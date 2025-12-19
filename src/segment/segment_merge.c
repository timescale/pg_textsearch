/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment_merge.c - Segment merge for LSM-style compaction
 */
#include <postgres.h>

#include <access/relation.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <utils/memutils.h>
#include <utils/timestamp.h>

#include "../constants.h"
#include "../metapage.h"
#include "docmap.h"
#include "fieldnorm.h"
#include "segment.h"
#include "segment_merge.h"

/*
 * Merge source state - tracks current position in each source segment
 * Updated for V2 segment format.
 */
typedef struct TpMergeSource
{
	TpSegmentReader *reader;		 /* Segment reader */
	uint32			 current_idx;	 /* Current term index in dictionary */
	uint32			 num_terms;		 /* Total terms in this segment */
	char			*current_term;	 /* Current term text (palloc'd) */
	TpDictEntryV2	 current_entry;	 /* V2 dictionary entry */
	bool			 exhausted;		 /* True if no more terms */
	uint32			*string_offsets; /* Cached string offsets array */
} TpMergeSource;

/*
 * Reference to a segment that contains a particular term.
 * Used to avoid loading all postings into memory during merge.
 */
typedef struct TpTermSegmentRef
{
	int			  segment_idx; /* Index into sources array */
	TpDictEntryV2 entry;	   /* V2 dict entry from that segment */
} TpTermSegmentRef;

/*
 * Merged term info - tracks which segments have this term.
 * Postings are streamed during write, not buffered in memory.
 */
typedef struct TpMergedTerm
{
	char			 *term;				/* Term text */
	uint32			  term_len;			/* Term length */
	TpTermSegmentRef *segment_refs;		/* Which segments have this term */
	uint32			  num_segment_refs; /* Number of segment refs */
	uint32			  segment_refs_capacity; /* Allocated capacity */
	/* Filled during posting write pass: */
	uint32 posting_offset; /* Absolute offset where postings start */
	uint32 posting_count;  /* Number of postings written */
} TpMergedTerm;

/*
 * Current posting info during merge (for N-way merge comparison).
 */
typedef struct TpMergePostingInfo
{
	ItemPointerData ctid;		/* CTID for comparison ordering */
	uint32			old_doc_id; /* Doc ID in source segment */
	uint16			frequency;	/* Term frequency */
	uint8			fieldnorm;	/* Encoded fieldnorm (1 byte) */
} TpMergePostingInfo;

/*
 * Posting merge source - tracks position in one segment's posting list
 * for streaming N-way merge. Updated for V2 block-based format.
 */
typedef struct TpPostingMergeSource
{
	TpSegmentReader	  *reader;	  /* Segment reader */
	TpMergePostingInfo current;	  /* Current posting info */
	bool			   exhausted; /* No more postings */

	/* V2 block iteration state */
	uint32			skip_index_offset; /* Offset to skip entries */
	uint16			block_count;	   /* Total blocks */
	uint32			current_block;	   /* Current block index */
	uint32			current_in_block;  /* Position within current block */
	TpSkipEntry		skip_entry;		   /* Current block's skip entry */
	TpBlockPosting *block_postings;	   /* Cached postings for current block */
	uint32			block_capacity;	   /* Allocated size of block_postings */
} TpPostingMergeSource;

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

	/* Read the V2 dictionary entry */
	tp_segment_read(
			source->reader,
			header->entries_offset +
					(source->current_idx * sizeof(TpDictEntryV2)),
			&source->current_entry,
			sizeof(TpDictEntryV2));

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
 * Add a segment reference to a merged term.
 * This records which segment has this term, without loading postings.
 */
static void
merged_term_add_segment_ref(
		TpMergedTerm *term, int segment_idx, TpDictEntryV2 *entry)
{
	/* Grow array if needed */
	if (term->num_segment_refs >= term->segment_refs_capacity)
	{
		uint32 new_capacity = term->segment_refs_capacity == 0
									? 8
									: term->segment_refs_capacity * 2;
		if (term->segment_refs == NULL)
			term->segment_refs = palloc(
					new_capacity * sizeof(TpTermSegmentRef));
		else
			term->segment_refs = repalloc(
					term->segment_refs,
					new_capacity * sizeof(TpTermSegmentRef));
		term->segment_refs_capacity = new_capacity;
	}

	term->segment_refs[term->num_segment_refs].segment_idx = segment_idx;
	term->segment_refs[term->num_segment_refs].entry	   = *entry;
	term->num_segment_refs++;
}

/*
 * Load the next block of postings for V2 merge source.
 * Returns true if a block was loaded, false if no more blocks remain.
 */
static bool
posting_source_load_block(TpPostingMergeSource *ps)
{
	uint32 skip_offset;

	if (ps->current_block >= ps->block_count)
		return false;

	/* Read skip entry for current block */
	skip_offset = ps->skip_index_offset +
				  (ps->current_block * sizeof(TpSkipEntry));
	tp_segment_read(
			ps->reader, skip_offset, &ps->skip_entry, sizeof(TpSkipEntry));

	/*
	 * Ensure we have enough buffer space. We reuse the buffer between blocks,
	 * only reallocating when a larger block is encountered. Old block data is
	 * no longer needed since we process blocks sequentially.
	 */
	if (ps->skip_entry.doc_count > ps->block_capacity)
	{
		if (ps->block_postings)
			pfree(ps->block_postings);
		ps->block_postings = palloc(
				ps->skip_entry.doc_count * sizeof(TpBlockPosting));
		ps->block_capacity = ps->skip_entry.doc_count;
	}

	/* Read posting data for this block */
	tp_segment_read(
			ps->reader,
			ps->skip_entry.posting_offset,
			ps->block_postings,
			ps->skip_entry.doc_count * sizeof(TpBlockPosting));

	ps->current_in_block = 0;
	return true;
}

/*
 * Convert current block posting to output format.
 */
static void
posting_source_convert_current(TpPostingMergeSource *ps)
{
	TpSegmentHeader *header = ps->reader->header;
	TpBlockPosting	*bp		= &ps->block_postings[ps->current_in_block];
	ItemPointerData	 ctid;

	/* Look up CTID from ctid_map (needed for N-way merge ordering) */
	tp_segment_read(
			ps->reader,
			header->ctid_map_offset + (bp->doc_id * sizeof(ItemPointerData)),
			&ctid,
			sizeof(ItemPointerData));

	/* Build output posting info */
	ps->current.ctid	   = ctid;
	ps->current.old_doc_id = bp->doc_id;
	ps->current.frequency  = bp->frequency;
	ps->current.fieldnorm  = bp->fieldnorm;
}

/*
 * Initialize a posting merge source for V2 streaming.
 */
static void
posting_source_init(
		TpPostingMergeSource *ps,
		TpSegmentReader		 *reader,
		TpDictEntryV2		 *entry)
{
	memset(ps, 0, sizeof(TpPostingMergeSource));
	ps->reader			  = reader;
	ps->skip_index_offset = entry->skip_index_offset;
	ps->block_count		  = entry->block_count;
	ps->current_block	  = 0;
	ps->current_in_block  = 0;
	ps->block_postings	  = NULL;
	ps->block_capacity	  = 0;
	ps->exhausted		  = (entry->block_count == 0);

	if (!ps->exhausted)
	{
		if (posting_source_load_block(ps))
		{
			posting_source_convert_current(ps);
		}
		else
		{
			ps->exhausted = true;
		}
	}
}

/*
 * Free posting merge source resources.
 */
static void
posting_source_free(TpPostingMergeSource *ps)
{
	if (ps->block_postings)
	{
		pfree(ps->block_postings);
		ps->block_postings = NULL;
	}
}

/*
 * Advance a posting merge source to the next posting.
 */
static bool
posting_source_advance(TpPostingMergeSource *ps)
{
	if (ps->exhausted)
		return false;

	ps->current_in_block++;

	/* Move to next block if current exhausted */
	while (ps->current_in_block >= ps->skip_entry.doc_count)
	{
		ps->current_block++;
		if (ps->current_block >= ps->block_count)
		{
			ps->exhausted = true;
			return false;
		}
		if (!posting_source_load_block(ps))
		{
			ps->exhausted = true;
			return false;
		}
	}

	posting_source_convert_current(ps);
	return true;
}

/*
 * Find the posting source with the smallest current CTID.
 * Returns -1 if all sources are exhausted.
 */
static int
find_min_posting_source(TpPostingMergeSource *sources, int num_sources)
{
	int				min_idx = -1;
	ItemPointerData min_ctid;
	int				i;

	for (i = 0; i < num_sources; i++)
	{
		ItemPointerData ctid;

		if (sources[i].exhausted)
			continue;

		/* Copy to avoid unaligned access from packed struct */
		memcpy(&ctid, &sources[i].current.ctid, sizeof(ItemPointerData));

		if (min_idx < 0 || ItemPointerCompare(&ctid, &min_ctid) < 0)
		{
			min_idx = i;
			memcpy(&min_ctid, &ctid, sizeof(ItemPointerData));
		}
	}

	return min_idx;
}

/*
 * Mapping from (source_idx, old_doc_id) → new_doc_id.
 * Avoids hash lookups during posting write phase.
 */
typedef struct TpMergeDocMapping
{
	uint32 **old_to_new; /* old_to_new[src_idx][old_doc_id] = new_doc_id */
	int		 num_sources;
} TpMergeDocMapping;

/*
 * Build merged docmap from source segment ctid_maps.
 * Also builds direct mapping arrays for fast old→new doc_id lookup.
 */
static TpDocMapBuilder *
build_merged_docmap(
		TpMergeSource *sources, int num_sources, TpMergeDocMapping *mapping)
{
	TpDocMapBuilder *docmap = tp_docmap_create();
	int				 i;

	/* Initialize mapping arrays */
	mapping->num_sources = num_sources;
	mapping->old_to_new	 = palloc0(num_sources * sizeof(uint32 *));

	for (i = 0; i < num_sources; i++)
	{
		TpSegmentHeader *header = sources[i].reader->header;
		uint32			 num_docs;
		uint32			 j;

		if (header->ctid_map_offset == 0)
			continue;

		num_docs = header->num_docs;

		/* Allocate mapping array for this source */
		mapping->old_to_new[i] = palloc(num_docs * sizeof(uint32));

		for (j = 0; j < num_docs; j++)
		{
			ItemPointerData ctid;
			uint8			fieldnorm;
			uint32			doc_length;
			uint32			new_doc_id;

			/* Read CTID from source */
			tp_segment_read(
					sources[i].reader,
					header->ctid_map_offset + (j * sizeof(ItemPointerData)),
					&ctid,
					sizeof(ItemPointerData));

			/* Read fieldnorm to get doc length */
			tp_segment_read(
					sources[i].reader,
					header->fieldnorm_offset + j,
					&fieldnorm,
					sizeof(uint8));
			doc_length = decode_fieldnorm(fieldnorm);

			/* Add to merged docmap and record mapping */
			new_doc_id = tp_docmap_add(docmap, &ctid, doc_length);
			mapping->old_to_new[i][j] = new_doc_id;
		}
	}

	tp_docmap_finalize(docmap);
	return docmap;
}

/*
 * Free merge doc mapping arrays.
 */
static void
free_merge_doc_mapping(TpMergeDocMapping *mapping)
{
	int i;

	for (i = 0; i < mapping->num_sources; i++)
	{
		if (mapping->old_to_new[i])
			pfree(mapping->old_to_new[i]);
	}
	pfree(mapping->old_to_new);
}

/*
 * Collected posting for V2 output.
 * Stores source segment info for direct mapping lookup (avoids hash lookup).
 */
typedef struct CollectedPosting
{
	ItemPointerData ctid;		/* For N-way merge ordering */
	int				source_idx; /* Index into sources array */
	uint32			old_doc_id; /* Doc ID in source segment */
	uint16			frequency;
	uint8			fieldnorm; /* Already encoded */
} CollectedPosting;

/*
 * Collect postings for a term using N-way merge.
 * Returns array of collected postings and sets *count.
 */
static CollectedPosting *
collect_term_postings(
		TpMergedTerm *term, TpMergeSource *sources, uint32 *count)
{
	TpPostingMergeSource *psources;
	int					  num_psources;
	uint32				  i;
	CollectedPosting	 *postings;
	uint32				  capacity = 64;
	uint32				  num	   = 0;

	*count = 0;

	if (term->num_segment_refs == 0)
		return NULL;

	/* Initialize posting sources */
	psources = palloc(sizeof(TpPostingMergeSource) * term->num_segment_refs);
	num_psources = term->num_segment_refs;

	for (i = 0; i < term->num_segment_refs; i++)
	{
		TpTermSegmentRef *ref	 = &term->segment_refs[i];
		TpMergeSource	 *source = &sources[ref->segment_idx];
		posting_source_init(&psources[i], source->reader, &ref->entry);
	}

	postings = palloc(sizeof(CollectedPosting) * capacity);

	/* N-way merge: find min, collect, advance */
	while (true)
	{
		int min_idx = find_min_posting_source(psources, num_psources);
		if (min_idx < 0)
			break;

		/* Grow array if needed */
		if (num >= capacity)
		{
			capacity *= 2;
			postings = repalloc(postings, sizeof(CollectedPosting) * capacity);
		}

		/* Store posting with source info for direct mapping lookup */
		postings[num].ctid		 = psources[min_idx].current.ctid;
		postings[num].source_idx = term->segment_refs[min_idx].segment_idx;
		postings[num].old_doc_id = psources[min_idx].current.old_doc_id;
		postings[num].frequency	 = psources[min_idx].current.frequency;
		postings[num].fieldnorm	 = psources[min_idx].current.fieldnorm;
		num++;

		posting_source_advance(&psources[min_idx]);
	}

	/* Free posting sources */
	for (i = 0; i < term->num_segment_refs; i++)
		posting_source_free(&psources[i]);
	pfree(psources);

	*count = num;
	return postings;
}

/*
 * Per-term block info for V2 merge output.
 */
typedef struct MergeTermBlockInfo
{
	uint32 skip_index_offset; /* Relative offset within skip index section */
	uint16 block_count;
	uint32 posting_offset; /* Relative offset within postings section */
	uint32 doc_freq;
} MergeTermBlockInfo;

/*
 * Write a merged segment in V2 format from an array of merged terms.
 *
 * Uses two passes:
 * 1. Collect postings per term and build docmap
 * 2. Write V2 format segment with block storage
 */
static BlockNumber
write_merged_segment(
		Relation	   index,
		TpMergedTerm  *terms,
		uint32		   num_terms,
		TpMergeSource *sources,
		int			   num_sources,
		uint32		   target_level,
		uint64		   total_tokens)
{
	TpSegmentWriter		writer;
	TpSegmentHeader		header;
	TpDictionary		dict;
	TpDocMapBuilder	   *docmap;
	TpMergeDocMapping	doc_mapping;
	MergeTermBlockInfo *term_blocks;
	uint32			   *string_offsets;
	uint32				string_pos;
	uint32				i;
	BlockNumber			header_block;
	BlockNumber			page_index_root;
	Buffer				header_buf;
	Page				header_page;
	TpSegmentHeader	   *existing_header;

	/* Per-term collected postings (for V2 block output) */
	CollectedPosting **term_postings;
	uint32			  *term_posting_counts;
	TpBlockPosting	 **all_block_postings;

	if (num_terms == 0)
		return InvalidBlockNumber;

	/* Build docmap and direct mapping arrays from source segments */
	docmap = build_merged_docmap(sources, num_sources, &doc_mapping);

	/* Collect postings for each term */
	term_postings		= palloc0(num_terms * sizeof(CollectedPosting *));
	term_posting_counts = palloc0(num_terms * sizeof(uint32));

	for (i = 0; i < num_terms; i++)
	{
		term_postings[i] = collect_term_postings(
				&terms[i], sources, &term_posting_counts[i]);
	}

	/* Initialize writer */
	memset(&writer, 0, sizeof(TpSegmentWriter));
	tp_segment_writer_init(&writer, index);
	header_block = writer.pages[0];

	/* Prepare header placeholder */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_V2;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= 0;
	header.num_terms	= num_terms;
	header.level		= target_level;
	header.next_segment = InvalidBlockNumber;
	header.num_docs		= docmap->num_docs;
	header.total_tokens = total_tokens;

	/* Write placeholder header */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Dictionary immediately follows header */
	header.dictionary_offset = writer.current_offset;

	/* Write dictionary header */
	dict.num_terms = num_terms;
	tp_segment_writer_write(
			&writer, &dict, offsetof(TpDictionary, string_offsets));

	/* Calculate string offsets */
	string_offsets = palloc0(num_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + terms[i].term_len + sizeof(uint32);
	}

	/* Write string offsets array */
	tp_segment_writer_write(
			&writer, string_offsets, num_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = writer.current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntryV2);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Calculate block info for each term */
	header.entries_offset = writer.current_offset;
	term_blocks			  = palloc0(num_terms * sizeof(MergeTermBlockInfo));

	{
		uint32 skip_offset	  = 0;
		uint32 posting_offset = 0;

		for (i = 0; i < num_terms; i++)
		{
			uint32 doc_count  = term_posting_counts[i];
			uint32 num_blocks = (doc_count + TP_BLOCK_SIZE - 1) /
								TP_BLOCK_SIZE;
			if (num_blocks == 0 && doc_count > 0)
				num_blocks = 1;

			term_blocks[i].skip_index_offset = skip_offset;
			term_blocks[i].block_count		 = (uint16)num_blocks;
			term_blocks[i].posting_offset	 = posting_offset;
			term_blocks[i].doc_freq			 = doc_count;

			skip_offset += num_blocks * sizeof(TpSkipEntry);
			posting_offset += doc_count * sizeof(TpBlockPosting);
		}
	}

	/* Calculate absolute offsets */
	header.skip_index_offset = header.entries_offset +
							   (num_terms * sizeof(TpDictEntryV2));

	{
		uint32 total_skip_size = 0;
		for (i = 0; i < num_terms; i++)
			total_skip_size += term_blocks[i].block_count *
							   sizeof(TpSkipEntry);

		header.postings_offset = header.skip_index_offset + total_skip_size;
	}

	/* Write V2 dictionary entries */
	for (i = 0; i < num_terms; i++)
	{
		TpDictEntryV2 entry;

		entry.skip_index_offset = header.skip_index_offset +
								  term_blocks[i].skip_index_offset;
		entry.block_count = term_blocks[i].block_count;
		entry.reserved	  = 0;
		entry.doc_freq	  = term_blocks[i].doc_freq;

		tp_segment_writer_write(&writer, &entry, sizeof(TpDictEntryV2));
	}

	/*
	 * Write skip index entries and posting blocks.
	 * We convert postings to block format once, storing for reuse.
	 */
	all_block_postings = palloc0(num_terms * sizeof(TpBlockPosting *));

	/* First pass: build block_postings and write skip entries */
	for (i = 0; i < num_terms; i++)
	{
		CollectedPosting *postings	= term_postings[i];
		uint32			  doc_count = term_posting_counts[i];
		uint32			  j;
		uint32			  block_idx;
		TpBlockPosting	 *block_postings;

		if (doc_count == 0)
			continue;

		/*
		 * Convert all postings to block format using direct mapping lookup.
		 * This avoids hash lookups by using precomputed old_doc_id→new_doc_id
		 * mapping arrays built during docmap construction.
		 */
		block_postings = palloc(doc_count * sizeof(TpBlockPosting));
		for (j = 0; j < doc_count; j++)
		{
			int	   src_idx	  = postings[j].source_idx;
			uint32 old_doc_id = postings[j].old_doc_id;
			uint32 new_doc_id = doc_mapping.old_to_new[src_idx][old_doc_id];

			block_postings[j].doc_id	= new_doc_id;
			block_postings[j].frequency = postings[j].frequency;
			block_postings[j].fieldnorm = postings[j].fieldnorm;
			block_postings[j].reserved	= 0;
		}
		all_block_postings[i] = block_postings;

		/* Write skip entries using converted block_postings */
		for (block_idx = 0; block_idx < term_blocks[i].block_count;
			 block_idx++)
		{
			TpSkipEntry skip;
			uint32		block_start = block_idx * TP_BLOCK_SIZE;
			uint32 block_end   = Min(block_start + TP_BLOCK_SIZE, doc_count);
			uint16 max_tf	   = 0;
			uint8  max_norm	   = 0;
			uint32 last_doc_id = 0;

			for (j = block_start; j < block_end; j++)
			{
				if (block_postings[j].doc_id > last_doc_id)
					last_doc_id = block_postings[j].doc_id;
				if (block_postings[j].frequency > max_tf)
					max_tf = block_postings[j].frequency;
				if (block_postings[j].fieldnorm > max_norm)
					max_norm = block_postings[j].fieldnorm;
			}

			skip.last_doc_id	= last_doc_id;
			skip.doc_count		= (uint8)(block_end - block_start);
			skip.block_max_tf	= max_tf;
			skip.block_max_norm = max_norm;
			skip.posting_offset = header.postings_offset +
								  term_blocks[i].posting_offset +
								  (block_start * sizeof(TpBlockPosting));
			skip.flags = TP_BLOCK_FLAG_UNCOMPRESSED;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			tp_segment_writer_write(&writer, &skip, sizeof(TpSkipEntry));
		}
	}

	/* Second pass: write posting blocks (reusing stored block_postings) */
	for (i = 0; i < num_terms; i++)
	{
		uint32 doc_count = term_posting_counts[i];

		if (all_block_postings[i] == NULL || doc_count == 0)
			continue;

		tp_segment_writer_write(
				&writer,
				all_block_postings[i],
				doc_count * sizeof(TpBlockPosting));

		pfree(all_block_postings[i]);
	}

	pfree(all_block_postings);

	/* Write fieldnorm table */
	header.fieldnorm_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer, docmap->fieldnorms, docmap->num_docs * sizeof(uint8));
	}

	/* Write CTID map */
	header.ctid_map_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_map,
				docmap->num_docs * sizeof(ItemPointerData));
	}

	header.doc_lengths_offset = writer.current_offset;

	/* Flush and write page index */
	tp_segment_writer_flush(&writer);

	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);
	header.page_index = page_index_root;
	header.data_size  = writer.current_offset;
	header.num_pages  = writer.pages_allocated;

	tp_segment_writer_finish(&writer);

	/* Update header on disk with final offsets */
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page		= BufferGetPage(header_buf);
	existing_header = (TpSegmentHeader *)((char *)header_page +
										  SizeOfPageHeaderData);

	existing_header->dictionary_offset	= header.dictionary_offset;
	existing_header->strings_offset		= header.strings_offset;
	existing_header->entries_offset		= header.entries_offset;
	existing_header->postings_offset	= header.postings_offset;
	existing_header->skip_index_offset	= header.skip_index_offset;
	existing_header->fieldnorm_offset	= header.fieldnorm_offset;
	existing_header->ctid_map_offset	= header.ctid_map_offset;
	existing_header->doc_lengths_offset = header.doc_lengths_offset;
	existing_header->num_docs			= header.num_docs;
	existing_header->data_size			= header.data_size;
	existing_header->num_pages			= header.num_pages;
	existing_header->page_index			= header.page_index;

	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	/* Cleanup */
	pfree(string_offsets);
	pfree(term_blocks);
	for (i = 0; i < num_terms; i++)
	{
		if (term_postings[i])
			pfree(term_postings[i]);
	}
	pfree(term_postings);
	pfree(term_posting_counts);
	free_merge_doc_mapping(&doc_mapping);
	tp_docmap_destroy(docmap);
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
	uint64			total_tokens	 = 0;
	BlockNumber		new_segment;
	MemoryContext	merge_ctx;
	MemoryContext	old_ctx;

	/* Page reclamation tracking (allocated outside merge context) */
	BlockNumber **segment_pages		   = NULL; /* Array of page arrays */
	uint32		 *segment_page_counts  = NULL; /* Count for each segment */
	uint32		  num_segments_tracked = 0;
	uint32		  total_pages_to_free  = 0;

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

	elog(DEBUG1, "Merging %u segments at level %u", segment_count, level);

	/*
	 * Allocate page tracking arrays in current context (not merge context)
	 * so they survive the merge context deletion.
	 */
	segment_pages		= palloc0(sizeof(BlockNumber *) * segment_count);
	segment_page_counts = palloc0(sizeof(uint32) * segment_count);

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
		uint64			 seg_tokens;

		reader = tp_segment_open(index, current);
		if (reader)
		{
			/* Get stats and next pointer, then close this reader */
			next	   = reader->header->next_segment;
			seg_tokens = reader->header->total_tokens;
			tp_segment_close(reader);

			/*
			 * Collect pages from this segment for later freeing.
			 * Allocate in parent context so it survives merge context delete.
			 */
			{
				MemoryContext save_ctx = MemoryContextSwitchTo(old_ctx);
				uint32		  page_count;

				page_count = tp_segment_collect_pages(
						index, current, &segment_pages[num_segments_tracked]);
				segment_page_counts[num_segments_tracked] = page_count;
				total_pages_to_free += page_count;
				num_segments_tracked++;

				MemoryContextSwitchTo(save_ctx);
			}

			/* Now init merge source (which opens its own reader) */
			if (merge_source_init(&sources[num_sources], index, current))
			{
				/* Accumulate corpus statistics only for successful inits */
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

		/* Clean up page tracking arrays */
		for (i = 0; i < (int)num_segments_tracked; i++)
		{
			if (segment_pages[i])
				pfree(segment_pages[i]);
		}
		pfree(segment_pages);
		pfree(segment_page_counts);

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
		current_merged					 = &merged_terms[num_merged_terms];
		current_merged->term_len		 = strlen(min_term);
		current_merged->term			 = pstrdup(min_term);
		current_merged->segment_refs	 = NULL;
		current_merged->num_segment_refs = 0;
		current_merged->segment_refs_capacity = 0;
		current_merged->posting_offset		  = 0; /* Set during write */
		current_merged->posting_count		  = 0; /* Set during write */
		num_merged_terms++;

		/* Record which segments have this term (don't load postings yet) */
		for (i = 0; i < num_sources; i++)
		{
			if (sources[i].exhausted)
				continue;

			if (strcmp(sources[i].current_term, min_term) == 0)
			{
				/* Record segment ref for later streaming merge */
				merged_term_add_segment_ref(
						current_merged, i, &sources[i].current_entry);

				/* Advance this source to next term */
				merge_source_advance(&sources[i]);
			}
		}

		/* Check for interrupt */
		CHECK_FOR_INTERRUPTS();
	}

	/* Write merged segment at next level (sources must remain open for
	 * streaming) */
	if (num_merged_terms > 0)
	{
		new_segment = write_merged_segment(
				index,
				merged_terms,
				num_merged_terms,
				sources,
				num_sources,
				level + 1,
				total_tokens);

		/* Free merged terms data */
		for (i = 0; i < (int)num_merged_terms; i++)
		{
			if (merged_terms[i].term)
				pfree(merged_terms[i].term);
			if (merged_terms[i].segment_refs)
				pfree(merged_terms[i].segment_refs);
		}
		pfree(merged_terms);
	}
	else
	{
		new_segment = InvalidBlockNumber;
	}

	/* Close all sources (after write_merged_segment is done with them) */
	for (i = 0; i < num_sources; i++)
	{
		merge_source_close(&sources[i]);
	}
	pfree(sources);

	/*
	 * Now that all source segment readers are closed (no more pinned buffers),
	 * flush dirty buffers to ensure merged segment is durable before updating
	 * the metapage.
	 */
	FlushRelationBuffers(index);

	MemoryContextSwitchTo(old_ctx);
	MemoryContextDelete(merge_ctx);

	if (new_segment == InvalidBlockNumber)
	{
		/* Clean up page tracking arrays on failure */
		for (i = 0; i < (int)num_segments_tracked; i++)
		{
			if (segment_pages[i])
				pfree(segment_pages[i]);
		}
		pfree(segment_pages);
		pfree(segment_page_counts);

		return InvalidBlockNumber;
	}

	/*
	 * Update metapage: clear source level, add to target level.
	 */
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

	/*
	 * Free pages from merged source segments. Now that the metapage no longer
	 * references these segments, their pages can be recycled via the FSM.
	 */
	for (i = 0; i < (int)num_segments_tracked; i++)
	{
		if (segment_pages[i] && segment_page_counts[i] > 0)
		{
			tp_segment_free_pages(
					index, segment_pages[i], segment_page_counts[i]);
		}
	}

	/*
	 * Update FSM upper-level pages so searches can find the freed pages.
	 * Without this, RecordFreeIndexPage only updates leaf pages, but
	 * GetFreeIndexPage searches from the root down.
	 */
	IndexFreeSpaceMapVacuum(index);

	elog(DEBUG1,
		 "Merged %u segments from L%u into L%u segment at block %u "
		 "(%u terms, freed %u pages)",
		 segment_count,
		 level,
		 level + 1,
		 new_segment,
		 num_merged_terms,
		 total_pages_to_free);

	/* Clean up page tracking arrays */
	for (i = 0; i < (int)num_segments_tracked; i++)
	{
		if (segment_pages[i])
			pfree(segment_pages[i]);
	}
	pfree(segment_pages);
	pfree(segment_page_counts);

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
