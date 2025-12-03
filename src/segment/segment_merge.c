/*-------------------------------------------------------------------------
 *
 * segment/segment_merge.c
 *    Segment merge implementation for LSM-style compaction
 *
 * This module implements N-way merge of segments at one level into a
 * single segment at the next level.
 *
 * IDENTIFICATION
 *    src/segment/segment_merge.c
 *
 *-------------------------------------------------------------------------
 */
#include "../constants.h"
#include "../metapage.h"
#include "access/relation.h"
#include "miscadmin.h"
#include "postgres.h"
#include "segment.h"
#include "segment_merge.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/* External GUC from mod.c */
extern int tp_segments_per_level;

/*
 * Merge source state - tracks current position in each source segment
 */
typedef struct TpMergeSource
{
	TpSegmentReader *reader;		 /* Segment reader */
	uint32			 current_idx;	 /* Current term index in dictionary */
	uint32			 num_terms;		 /* Total terms in this segment */
	char			*current_term;	 /* Current term text (palloc'd) */
	TpDictEntry		 current_entry;	 /* Current dictionary entry */
	bool			 exhausted;		 /* True if no more terms */
	uint32			*string_offsets; /* Cached string offsets array */
} TpMergeSource;

/*
 * Merged posting entry - used for combining postings from multiple segments
 */
typedef struct TpMergedPosting
{
	ItemPointerData ctid;
	uint16			frequency;
	uint16			doc_length;
} TpMergedPosting;

/*
 * Merged term info - accumulates data for one term across all sources
 */
typedef struct TpMergedTerm
{
	char			*term;				/* Term text */
	uint32			 term_len;			/* Term length */
	TpMergedPosting *postings;			/* Array of merged postings */
	uint32			 num_postings;		/* Number of postings */
	uint32			 postings_capacity; /* Allocated capacity */
	uint32			 doc_freq;			/* Document frequency (unique docs) */
} TpMergedTerm;

/* Data bytes per segment page (consistent with segment.c) */
#define SEGMENT_DATA_PER_PAGE (BLCKSZ - SizeOfPageHeaderData)

/*
 * Read term at index from a segment's dictionary.
 * Returns palloc'd string that must be freed by caller.
 */
static char *
merge_read_term_at_index(TpMergeSource *source, uint32 index)
{
	TpSegmentHeader *header = source->reader->header;
	uint32			 string_offset;
	uint32			 length;
	char			*term;

	string_offset = header->strings_offset + source->string_offsets[index];

	/* Read string length */
	tp_segment_read(source->reader, string_offset, &length, sizeof(uint32));

	/* Allocate and read string */
	term = palloc(length + 1);
	tp_segment_read(
			source->reader, string_offset + sizeof(uint32), term, length);
	term[length] = '\0';

	return term;
}

/*
 * Advance a merge source to its next term.
 * Returns false if source is exhausted.
 */
static bool
merge_source_advance(TpMergeSource *source)
{
	TpSegmentHeader *header;

	if (source->exhausted)
		return false;

	/* Free previous term if any */
	if (source->current_term)
	{
		pfree(source->current_term);
		source->current_term = NULL;
	}

	source->current_idx++;

	if (source->current_idx >= source->num_terms)
	{
		source->exhausted = true;
		return false;
	}

	header = source->reader->header;

	/* Read the term at current index */
	source->current_term =
			merge_read_term_at_index(source, source->current_idx);

	/* Read the dictionary entry */
	tp_segment_read(
			source->reader,
			header->entries_offset +
					(source->current_idx * sizeof(TpDictEntry)),
			&source->current_entry,
			sizeof(TpDictEntry));

	return true;
}

/*
 * Initialize a merge source for a segment.
 * Returns false if segment is empty or invalid.
 */
static bool
merge_source_init(TpMergeSource *source, Relation index, BlockNumber root)
{
	TpSegmentHeader *header;
	TpDictionary	 dict_header;

	memset(source, 0, sizeof(TpMergeSource));
	source->exhausted = true; /* Assume failure */

	source->reader = tp_segment_open(index, root);
	if (!source->reader)
		return false;

	header = source->reader->header;

	if (header->num_terms == 0)
	{
		tp_segment_close(source->reader);
		source->reader = NULL;
		return false;
	}

	source->num_terms = header->num_terms;

	/* Read dictionary header */
	tp_segment_read(
			source->reader,
			header->dictionary_offset,
			&dict_header,
			sizeof(dict_header.num_terms));

	/* Cache all string offsets for this segment */
	source->string_offsets = palloc(sizeof(uint32) * source->num_terms);
	tp_segment_read(
			source->reader,
			header->dictionary_offset + sizeof(dict_header.num_terms),
			source->string_offsets,
			sizeof(uint32) * source->num_terms);

	/* Position before first term (advance will move to index 0) */
	source->current_idx	 = UINT32_MAX; /* Will wrap to 0 on advance */
	source->exhausted	 = false;
	source->current_term = NULL;

	/* Advance to first term */
	if (!merge_source_advance(source))
	{
		tp_segment_close(source->reader);
		source->reader = NULL;
		pfree(source->string_offsets);
		source->string_offsets = NULL;
		return false;
	}

	return true;
}

/*
 * Close and cleanup a merge source.
 */
static void
merge_source_close(TpMergeSource *source)
{
	if (source->current_term)
	{
		pfree(source->current_term);
		source->current_term = NULL;
	}
	if (source->string_offsets)
	{
		pfree(source->string_offsets);
		source->string_offsets = NULL;
	}
	if (source->reader)
	{
		tp_segment_close(source->reader);
		source->reader = NULL;
	}
}

/*
 * Find the source with the lexicographically smallest current term.
 * Returns -1 if all sources are exhausted.
 */
static int
merge_find_min_source(TpMergeSource *sources, int num_sources)
{
	int			min_idx	 = -1;
	const char *min_term = NULL;
	int			i;

	for (i = 0; i < num_sources; i++)
	{
		if (sources[i].exhausted)
			continue;

		if (min_idx < 0 || strcmp(sources[i].current_term, min_term) < 0)
		{
			min_idx	 = i;
			min_term = sources[i].current_term;
		}
	}

	return min_idx;
}

/*
 * Add a posting to the merged term, avoiding duplicates.
 * If a duplicate CTID is found, keep the higher frequency.
 */
static void
merged_term_add_posting(
		TpMergedTerm	*term,
		ItemPointerData *ctid,
		uint16			 frequency,
		uint16			 doc_length)
{
	uint32 i;

	/* Check for duplicate CTID */
	for (i = 0; i < term->num_postings; i++)
	{
		if (ItemPointerEquals(&term->postings[i].ctid, ctid))
		{
			/* Keep higher frequency */
			if (frequency > term->postings[i].frequency)
				term->postings[i].frequency = frequency;
			return;
		}
	}

	/* Grow array if needed */
	if (term->num_postings >= term->postings_capacity)
	{
		uint32 new_capacity = term->postings_capacity == 0
									? 64
									: term->postings_capacity * 2;
		/* PostgreSQL's repalloc doesn't handle NULL - use palloc first time */
		if (term->postings == NULL)
			term->postings = palloc(new_capacity * sizeof(TpMergedPosting));
		else
			term->postings = repalloc(
					term->postings, new_capacity * sizeof(TpMergedPosting));
		term->postings_capacity = new_capacity;
	}

	/* Add new posting */
	term->postings[term->num_postings].ctid		  = *ctid;
	term->postings[term->num_postings].frequency  = frequency;
	term->postings[term->num_postings].doc_length = doc_length;
	term->num_postings++;
	term->doc_freq++;
}

/*
 * Collect all postings for a term from a source segment.
 */
static void
merge_collect_postings(TpMergeSource *source, TpMergedTerm *merged)
{
	TpSegmentPosting posting;
	ItemPointerData	 ctid_copy;
	uint32			 i;

	for (i = 0; i < source->current_entry.posting_count; i++)
	{
		/* Read posting from segment */
		tp_segment_read(
				source->reader,
				source->current_entry.posting_offset +
						(i * sizeof(TpSegmentPosting)),
				&posting,
				sizeof(TpSegmentPosting));

		/* Copy ctid to avoid unaligned access from packed struct */
		memcpy(&ctid_copy, &posting.ctid, sizeof(ItemPointerData));
		merged_term_add_posting(
				merged, &ctid_copy, posting.frequency, posting.doc_length);
	}
}

/*
 * Compare function for sorting postings by CTID.
 */
static int
posting_cmp(const void *a, const void *b)
{
	TpMergedPosting *pa = (TpMergedPosting *)a;
	TpMergedPosting *pb = (TpMergedPosting *)b;

	return ItemPointerCompare(&pa->ctid, &pb->ctid);
}

/*
 * Write a merged segment from an array of merged terms.
 * This is similar to tp_write_segment() but takes pre-built terms.
 */
static BlockNumber
write_merged_segment(
		Relation	  index,
		TpMergedTerm *terms,
		uint32		  num_terms,
		uint32		  target_level,
		uint32		  total_docs,
		uint64		  total_tokens)
{
	TpSegmentWriter	 writer;
	TpSegmentHeader	 header;
	TpDictionary	 dict;
	uint32			*string_offsets;
	uint32			*posting_offsets;
	uint32			 string_pos;
	uint32			 current_offset;
	uint32			 i;
	BlockNumber		 header_block;
	BlockNumber		 page_index_root;
	Buffer			 header_buf;
	Page			 header_page;
	TpSegmentHeader *existing_header;

	if (num_terms == 0)
		return InvalidBlockNumber;

	/* Initialize writer */
	memset(&writer, 0, sizeof(TpSegmentWriter));
	tp_segment_writer_init(&writer, index);
	header_block = writer.pages[0];

	/* Prepare header */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic			 = TP_SEGMENT_MAGIC;
	header.version			 = TP_SEGMENT_VERSION;
	header.created_at		 = GetCurrentTimestamp();
	header.num_pages		 = 0;
	header.num_terms		 = num_terms;
	header.level			 = target_level;
	header.next_segment		 = InvalidBlockNumber;
	header.num_docs			 = total_docs;
	header.total_tokens		 = total_tokens;
	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Write placeholder header */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary section */
	dict.num_terms = num_terms;
	tp_segment_writer_write(
			&writer, &dict, offsetof(TpDictionary, string_offsets));

	/* Calculate offsets */
	string_offsets	= palloc0(num_terms * sizeof(uint32));
	posting_offsets = palloc0(num_terms * sizeof(uint32));

	string_pos	   = 0;
	current_offset = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + terms[i].term_len + sizeof(uint32);

		posting_offsets[i] = current_offset;
		current_offset += sizeof(TpSegmentPosting) * terms[i].num_postings;
	}

	/* Write string offsets */
	tp_segment_writer_write(
			&writer, string_offsets, num_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = writer.current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Calculate postings base offset */
	header.entries_offset  = writer.current_offset;
	header.postings_offset = writer.current_offset +
							 (num_terms * sizeof(TpDictEntry));

	/* Write dictionary entries */
	for (i = 0; i < num_terms; i++)
	{
		TpDictEntry entry;

		entry.posting_offset = header.postings_offset + posting_offsets[i];
		entry.posting_count	 = terms[i].num_postings;
		entry.doc_freq		 = terms[i].doc_freq;

		tp_segment_writer_write(&writer, &entry, sizeof(TpDictEntry));
	}

	/* Write posting lists */
	for (i = 0; i < num_terms; i++)
	{
		uint32 j;

		if (terms[i].num_postings > 0)
		{
			/* Sort postings by CTID for efficient lookup */
			qsort(terms[i].postings,
				  terms[i].num_postings,
				  sizeof(TpMergedPosting),
				  posting_cmp);

			/* Convert to segment format and write */
			for (j = 0; j < terms[i].num_postings; j++)
			{
				TpSegmentPosting seg_posting;

				seg_posting.ctid	   = terms[i].postings[j].ctid;
				seg_posting.frequency  = terms[i].postings[j].frequency;
				seg_posting.doc_length = terms[i].postings[j].doc_length;

				tp_segment_writer_write(
						&writer, &seg_posting, sizeof(TpSegmentPosting));
			}
		}
	}

	/* No doc_lengths section needed for merged segments - info is inline */
	header.doc_lengths_offset = writer.current_offset;

	/* Flush and write page index */
	tp_segment_writer_flush(&writer);

	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);
	header.page_index = page_index_root;
	header.data_size  = writer.current_offset;
	header.num_pages  = writer.pages_allocated;

	tp_segment_writer_finish(&writer);

	FlushRelationBuffers(index);

	/* Update header on disk */
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page		= BufferGetPage(header_buf);
	existing_header = (TpSegmentHeader *)((char *)header_page +
										  SizeOfPageHeaderData);

	existing_header->strings_offset		= header.strings_offset;
	existing_header->entries_offset		= header.entries_offset;
	existing_header->postings_offset	= header.postings_offset;
	existing_header->doc_lengths_offset = header.doc_lengths_offset;
	existing_header->num_docs			= header.num_docs;
	existing_header->data_size			= header.data_size;
	existing_header->num_pages			= header.num_pages;
	existing_header->page_index			= header.page_index;

	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	FlushRelationBuffers(index);

	/* Cleanup */
	pfree(string_offsets);
	pfree(posting_offsets);
	if (writer.pages)
		pfree(writer.pages);

	return header_block;
}

/*
 * Merge all segments at the specified level into a single segment at level+1.
 */
BlockNumber
tp_merge_level_segments(Relation index, uint32 level)
{
	TpIndexMetaPage metap;
	Buffer			metabuf;
	Page			metapage;
	BlockNumber		first_segment;
	uint32			segment_count;
	TpMergeSource  *sources;
	int				num_sources;
	int				i;
	BlockNumber		current;
	TpMergedTerm   *merged_terms	 = NULL;
	uint32			num_merged_terms = 0;
	uint32			merged_capacity	 = 0;
	uint32			total_docs		 = 0;
	uint64			total_tokens	 = 0;
	BlockNumber		new_segment;
	MemoryContext	merge_ctx;
	MemoryContext	old_ctx;

	if (level >= TP_MAX_LEVELS - 1)
	{
		elog(WARNING,
			 "Cannot merge level %u - would exceed TP_MAX_LEVELS",
			 level);
		return InvalidBlockNumber;
	}

	/* Read metapage to get segment chain */
	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	first_segment = metap->level_heads[level];
	segment_count = metap->level_counts[level];

	UnlockReleaseBuffer(metabuf);

	if (first_segment == InvalidBlockNumber || segment_count == 0)
	{
		return InvalidBlockNumber;
	}

	elog(NOTICE, "Merging %u segments at level %u", segment_count, level);

	/* Create memory context for merge operation */
	merge_ctx = AllocSetContextCreate(
			CurrentMemoryContext, "Segment Merge", ALLOCSET_DEFAULT_SIZES);
	old_ctx = MemoryContextSwitchTo(merge_ctx);

	/* Allocate array for merge sources */
	sources		= palloc0(sizeof(TpMergeSource) * segment_count);
	num_sources = 0;

	/* Open all segments in the chain */
	current = first_segment;
	while (current != InvalidBlockNumber && num_sources < (int)segment_count)
	{
		TpSegmentReader *reader;
		BlockNumber		 next;
		uint32			 seg_docs;
		uint64			 seg_tokens;

		reader = tp_segment_open(index, current);
		if (reader)
		{
			/* Get stats and next pointer, then close this reader */
			next	   = reader->header->next_segment;
			seg_docs   = reader->header->num_docs;
			seg_tokens = reader->header->total_tokens;
			tp_segment_close(reader);

			/* Now init merge source (which opens its own reader) */
			if (merge_source_init(&sources[num_sources], index, current))
			{
				/* Accumulate corpus statistics only for successful inits */
				total_docs += seg_docs;
				total_tokens += seg_tokens;
				num_sources++;
			}

			current = next;
		}
		else
		{
			break;
		}
	}

	if (num_sources == 0)
	{
		MemoryContextSwitchTo(old_ctx);
		MemoryContextDelete(merge_ctx);
		return InvalidBlockNumber;
	}

	/* Perform N-way merge */
	while (true)
	{
		int			  min_idx;
		const char	 *min_term;
		TpMergedTerm *current_merged;

		/* Find source with smallest term */
		min_idx = merge_find_min_source(sources, num_sources);
		if (min_idx < 0)
			break; /* All sources exhausted */

		min_term = sources[min_idx].current_term;

		/* Grow merged terms array if needed */
		if (num_merged_terms >= merged_capacity)
		{
			merged_capacity = merged_capacity == 0 ? 1024
												   : merged_capacity * 2;
			/* PostgreSQL's repalloc doesn't handle NULL - use palloc first */
			if (merged_terms == NULL)
				merged_terms = palloc(merged_capacity * sizeof(TpMergedTerm));
			else
				merged_terms = repalloc(
						merged_terms, merged_capacity * sizeof(TpMergedTerm));
		}

		/* Initialize new merged term */
		current_merged					  = &merged_terms[num_merged_terms];
		current_merged->term_len		  = strlen(min_term);
		current_merged->term			  = pstrdup(min_term);
		current_merged->postings		  = NULL;
		current_merged->num_postings	  = 0;
		current_merged->postings_capacity = 0;
		current_merged->doc_freq		  = 0;
		num_merged_terms++;

		/* Collect postings from all sources that have this term */
		for (i = 0; i < num_sources; i++)
		{
			if (sources[i].exhausted)
				continue;

			if (strcmp(sources[i].current_term, min_term) == 0)
			{
				/* Collect all postings from this source for this term */
				merge_collect_postings(&sources[i], current_merged);

				/* Advance this source to next term */
				merge_source_advance(&sources[i]);
			}
		}

		/* Check for interrupt */
		CHECK_FOR_INTERRUPTS();
	}

	/* Close all sources */
	for (i = 0; i < num_sources; i++)
	{
		merge_source_close(&sources[i]);
	}
	pfree(sources);

	/* Write merged segment at next level */
	if (num_merged_terms > 0)
	{
		new_segment = write_merged_segment(
				index,
				merged_terms,
				num_merged_terms,
				level + 1,
				total_docs,
				total_tokens);

		/* Free merged terms data */
		for (i = 0; i < (int)num_merged_terms; i++)
		{
			if (merged_terms[i].term)
				pfree(merged_terms[i].term);
			if (merged_terms[i].postings)
				pfree(merged_terms[i].postings);
		}
		pfree(merged_terms);
	}
	else
	{
		new_segment = InvalidBlockNumber;
	}

	MemoryContextSwitchTo(old_ctx);
	MemoryContextDelete(merge_ctx);

	if (new_segment == InvalidBlockNumber)
	{
		return InvalidBlockNumber;
	}

	/* Update metapage: clear source level, add to target level */
	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	/* Clear source level */
	metap->level_heads[level]  = InvalidBlockNumber;
	metap->level_counts[level] = 0;

	/* Add merged segment to target level */
	if (metap->level_heads[level + 1] != InvalidBlockNumber)
	{
		/* Link to existing chain */
		Buffer			 seg_buf;
		Page			 seg_page;
		TpSegmentHeader *seg_header;

		seg_buf = ReadBuffer(index, new_segment);
		LockBuffer(seg_buf, BUFFER_LOCK_EXCLUSIVE);
		seg_page				 = BufferGetPage(seg_buf);
		seg_header				 = (TpSegmentHeader *)((char *)seg_page +
										   SizeOfPageHeaderData);
		seg_header->next_segment = metap->level_heads[level + 1];
		MarkBufferDirty(seg_buf);
		UnlockReleaseBuffer(seg_buf);
	}

	metap->level_heads[level + 1] = new_segment;
	metap->level_counts[level + 1]++;

	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	elog(NOTICE,
		 "Merged %u segments from L%u into L%u segment at block %u "
		 "(%u terms)",
		 segment_count,
		 level,
		 level + 1,
		 new_segment,
		 num_merged_terms);

	return new_segment;
}

/*
 * Check if a level needs compaction and trigger merge if so.
 */
void
tp_maybe_compact_level(Relation index, uint32 level)
{
	TpIndexMetaPage metap;
	Buffer			metabuf;
	Page			metapage;
	uint16			level_count;

	if (level >= TP_MAX_LEVELS - 1)
		return;

	/* Check if level needs compaction */
	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	level_count = metap->level_counts[level];

	UnlockReleaseBuffer(metabuf);

	if (level_count < (uint16)tp_segments_per_level)
		return; /* Level not full */

	/* Merge this level */
	if (tp_merge_level_segments(index, level) != InvalidBlockNumber)
	{
		/* Recursively check next level */
		tp_maybe_compact_level(index, level + 1);
	}
}
