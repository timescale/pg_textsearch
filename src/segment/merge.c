/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * merge.c - Segment merge for LSM-style compaction
 */
#include <postgres.h>

#include <access/relation.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <utils/memutils.h>
#include <utils/timestamp.h>

#include "compression.h"
#include "constants.h"
#include "docmap.h"
#include "fieldnorm.h"
#include "merge.h"
#include "pagemapper.h"
#include "segment.h"
#include "state/metapage.h"

/* GUC variable for segment compression */
extern bool tp_compress_segments;

/*
 * Merge source state - tracks current position in each source segment
 * Updated for segment format.
 */
typedef struct TpMergeSource
{
	TpSegmentReader *reader;		 /* Segment reader */
	uint32			 current_idx;	 /* Current term index in dictionary */
	uint32			 num_terms;		 /* Total terms in this segment */
	char			*current_term;	 /* Current term text (palloc'd) */
	TpDictEntry		 current_entry;	 /* dictionary entry */
	bool			 exhausted;		 /* True if no more terms */
	uint32			*string_offsets; /* Cached string offsets array */
} TpMergeSource;

/*
 * Reference to a segment that contains a particular term.
 * Used to avoid loading all postings into memory during merge.
 */
typedef struct TpTermSegmentRef
{
	int			segment_idx; /* Index into sources array */
	TpDictEntry entry;		 /* dict entry from that segment */
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
	uint64 posting_offset; /* Absolute offset where postings start */
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
 * for streaming N-way merge. Updated for block-based format.
 */
typedef struct TpPostingMergeSource
{
	TpSegmentReader	  *reader;	  /* Segment reader */
	TpMergePostingInfo current;	  /* Current posting info */
	bool			   exhausted; /* No more postings */

	/* block iteration state */
	uint64			skip_index_offset; /* Offset to skip entries */
	uint16			block_count;	   /* Total blocks */
	uint32			current_block;	   /* Current block index */
	uint32			current_in_block;  /* Position within current block */
	TpSkipEntry		skip_entry;		   /* Current block's skip entry */
	TpBlockPosting *block_postings;	   /* Cached postings for current block */
	uint32			block_capacity;	   /* Allocated size of block_postings */
} TpPostingMergeSource;

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

	/* Read the dictionary entry (version-aware) */
	tp_segment_read_dict_entry(
			source->reader,
			header,
			source->current_idx,
			&source->current_entry);

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
		TpMergedTerm *term, int segment_idx, TpDictEntry *entry)
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
 * Load the next block of postings for merge source.
 * Returns true if a block was loaded, false if no more blocks remain.
 */
static bool
posting_source_load_block(TpPostingMergeSource *ps)
{
	if (ps->current_block >= ps->block_count)
		return false;

	/* Read skip entry for current block (version-aware) */
	{
		TpDictEntry tmp_dict;
		tmp_dict.skip_index_offset = ps->skip_index_offset;
		tp_segment_read_skip_entry(
				ps->reader, &tmp_dict, ps->current_block, &ps->skip_entry);
	}

	/*
	 * Ensure we have enough buffer space. We reuse the buffer between blocks,
	 * only reallocating when a larger block is encountered. Old block data is
	 * no longer needed since we process blocks sequentially.
	 *
	 * The NULL check ensures allocation on first call (when block_postings is
	 * uninitialized) even if doc_count happens to equal block_capacity.
	 */
	if (ps->block_postings == NULL ||
		ps->skip_entry.doc_count > ps->block_capacity)
	{
		if (ps->block_postings)
			pfree(ps->block_postings);
		ps->block_postings = palloc(
				ps->skip_entry.doc_count * sizeof(TpBlockPosting));
		ps->block_capacity = ps->skip_entry.doc_count;
	}

	/* Read posting data for this block (handle compression) */
	if (ps->skip_entry.flags == TP_BLOCK_FLAG_DELTA)
	{
		/* Compressed block - read and decompress */
		uint8 compressed_buf[TP_MAX_COMPRESSED_BLOCK_SIZE];

		tp_segment_read(
				ps->reader,
				ps->skip_entry.posting_offset,
				compressed_buf,
				TP_MAX_COMPRESSED_BLOCK_SIZE);

		tp_decompress_block(
				compressed_buf,
				ps->skip_entry.doc_count,
				0, /* first_doc_id - deltas are relative within block */
				ps->block_postings);
	}
	else
	{
		/* Uncompressed block - read directly */
		tp_segment_read(
				ps->reader,
				ps->skip_entry.posting_offset,
				ps->block_postings,
				ps->skip_entry.doc_count * sizeof(TpBlockPosting));
	}

	ps->current_in_block = 0;
	return true;
}

/*
 * Convert current block posting to output format.
 */
static void
posting_source_convert_current(TpPostingMergeSource *ps)
{
	TpBlockPosting *bp = &ps->block_postings[ps->current_in_block];
	BlockNumber		page;
	OffsetNumber	offset;

	/*
	 * Look up CTID from split arrays (needed for N-way merge ordering).
	 * Use cached arrays if available, otherwise read from segment.
	 */
	if (ps->reader->cached_ctid_pages != NULL)
	{
		page   = ps->reader->cached_ctid_pages[bp->doc_id];
		offset = ps->reader->cached_ctid_offsets[bp->doc_id];
	}
	else
	{
		TpSegmentHeader *header = ps->reader->header;

		tp_segment_read(
				ps->reader,
				header->ctid_pages_offset + (bp->doc_id * sizeof(BlockNumber)),
				&page,
				sizeof(BlockNumber));
		tp_segment_read(
				ps->reader,
				header->ctid_offsets_offset +
						(bp->doc_id * sizeof(OffsetNumber)),
				&offset,
				sizeof(OffsetNumber));
	}

	/* Build output posting info */
	ItemPointerSet(&ps->current.ctid, page, offset);
	ps->current.old_doc_id = bp->doc_id;
	ps->current.frequency  = bp->frequency;
	ps->current.fieldnorm  = bp->fieldnorm;
}

/*
 * Initialize a posting merge source for streaming.
 */
static void
posting_source_init(
		TpPostingMergeSource *ps, TpSegmentReader *reader, TpDictEntry *entry)
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
 * Build merged docmap from source segment CTID arrays.
 * Also builds direct mapping arrays for fast old→new doc_id lookup.
 *
 * IMPORTANT: The mapping is built AFTER finalize because finalize
 * reassigns doc_ids in CTID order (the docmap invariant).
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

	/* First pass: add all documents to docmap (doc_ids are temporary) */
	for (i = 0; i < num_sources; i++)
	{
		TpSegmentHeader *header = sources[i].reader->header;
		uint32			 num_docs;
		uint32			 j;

		if (header->ctid_pages_offset == 0)
			continue;

		num_docs = header->num_docs;

		/* Allocate mapping array for this source */
		mapping->old_to_new[i] = palloc(num_docs * sizeof(uint32));

		for (j = 0; j < num_docs; j++)
		{
			ItemPointerData ctid;
			BlockNumber		page;
			OffsetNumber	offset;
			uint8			fieldnorm;
			uint32			doc_length;

			/*
			 * Read CTID from split arrays.
			 * Use cached arrays if available.
			 */
			if (sources[i].reader->cached_ctid_pages != NULL)
			{
				page   = sources[i].reader->cached_ctid_pages[j];
				offset = sources[i].reader->cached_ctid_offsets[j];
			}
			else
			{
				tp_segment_read(
						sources[i].reader,
						header->ctid_pages_offset + (j * sizeof(BlockNumber)),
						&page,
						sizeof(BlockNumber));
				tp_segment_read(
						sources[i].reader,
						header->ctid_offsets_offset +
								(j * sizeof(OffsetNumber)),
						&offset,
						sizeof(OffsetNumber));
			}
			ItemPointerSet(&ctid, page, offset);

			/* Read fieldnorm to get doc length */
			tp_segment_read(
					sources[i].reader,
					header->fieldnorm_offset + j,
					&fieldnorm,
					sizeof(uint8));
			doc_length = decode_fieldnorm(fieldnorm);

			/* Add to merged docmap (doc_id assigned here is temporary) */
			tp_docmap_add(docmap, &ctid, doc_length);
		}
	}

	/* Finalize: reassigns doc_ids in CTID order */
	tp_docmap_finalize(docmap);

	/*
	 * Second pass: build old→new mapping using finalized doc_ids.
	 * After finalize, tp_docmap_lookup returns the correct CTID-sorted doc_id.
	 */
	for (i = 0; i < num_sources; i++)
	{
		TpSegmentHeader *header = sources[i].reader->header;
		uint32			 num_docs;
		uint32			 j;

		if (header->ctid_pages_offset == 0 || mapping->old_to_new[i] == NULL)
			continue;

		num_docs = header->num_docs;

		for (j = 0; j < num_docs; j++)
		{
			ItemPointerData ctid;
			BlockNumber		page;
			OffsetNumber	offset;

			/* Reconstruct CTID for lookup */
			if (sources[i].reader->cached_ctid_pages != NULL)
			{
				page   = sources[i].reader->cached_ctid_pages[j];
				offset = sources[i].reader->cached_ctid_offsets[j];
			}
			else
			{
				tp_segment_read(
						sources[i].reader,
						header->ctid_pages_offset + (j * sizeof(BlockNumber)),
						&page,
						sizeof(BlockNumber));
				tp_segment_read(
						sources[i].reader,
						header->ctid_offsets_offset +
								(j * sizeof(OffsetNumber)),
						&offset,
						sizeof(OffsetNumber));
			}
			ItemPointerSet(&ctid, page, offset);

			/* Look up the finalized doc_id */
			mapping->old_to_new[i][j] = tp_docmap_lookup(docmap, &ctid);
		}
	}

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
 * Initialize N-way merge posting sources for a term.
 * Returns the number of sources initialized.
 */
static TpPostingMergeSource *
init_term_posting_sources(
		TpMergedTerm *term, TpMergeSource *sources, int *num_psources)
{
	TpPostingMergeSource *psources;
	uint32				  i;

	*num_psources = term->num_segment_refs;
	psources = palloc(sizeof(TpPostingMergeSource) * term->num_segment_refs);

	for (i = 0; i < term->num_segment_refs; i++)
	{
		TpTermSegmentRef *ref	 = &term->segment_refs[i];
		TpMergeSource	 *source = &sources[ref->segment_idx];

		posting_source_init(&psources[i], source->reader, &ref->entry);
	}

	return psources;
}

/*
 * Free N-way merge posting sources for a term.
 */
static void
free_term_posting_sources(TpPostingMergeSource *psources, int num_psources)
{
	int i;

	for (i = 0; i < num_psources; i++)
		posting_source_free(&psources[i]);
	pfree(psources);
}

/*
 * Per-term block info for merge output.
 * Updated for streaming format: postings written before skip index.
 */
typedef struct MergeTermBlockInfo
{
	uint64 posting_offset;	 /* Absolute offset where postings were written */
	uint16 block_count;		 /* Number of blocks for this term */
	uint32 doc_freq;		 /* Document frequency */
	uint32 skip_entry_start; /* Index into accumulated skip entries array */
} MergeTermBlockInfo;

/*
 * Write a merged segment in streaming format.
 *
 * Layout: [header] → [dictionary] → [postings] → [skip index] →
 *         [fieldnorm] → [ctid map]
 *
 * This enables streaming writes: postings are written before skip index,
 * so we can stream per-term without loading all postings into memory.
 * Skip entries are accumulated in memory (small: 16 bytes × total blocks)
 * and written after all postings.
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

	/* Accumulated skip entries for all terms */
	TpSkipEntry *all_skip_entries;
	uint32		 skip_entries_count;
	uint32		 skip_entries_capacity;

	if (num_terms == 0)
		return InvalidBlockNumber;

	/* Build docmap and direct mapping arrays from source segments */
	docmap = build_merged_docmap(sources, num_sources, &doc_mapping);

	/* Initialize writer */
	memset(&writer, 0, sizeof(TpSegmentWriter));
	tp_segment_writer_init(&writer, index);
	header_block = writer.pages[0];

	/* Prepare header placeholder */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_VERSION;
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
	memset(&dict, 0, sizeof(dict));
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
		uint32 dict_offset = i * sizeof(TpDictEntry);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Record entries offset - dict entries written after postings loop */
	header.entries_offset = writer.current_offset;

	/* Write placeholder dict entries - we'll fill them in after streaming */
	{
		TpDictEntry placeholder;
		memset(&placeholder, 0, sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
			tp_segment_writer_write(
					&writer, &placeholder, sizeof(TpDictEntry));
	}

	/* Postings start here - streaming format writes postings first */
	header.postings_offset = writer.current_offset;

	/* Initialize per-term tracking and skip entry accumulator */
	term_blocks = palloc0(num_terms * sizeof(MergeTermBlockInfo));

	skip_entries_capacity = 1024;
	skip_entries_count	  = 0;
	all_skip_entries = palloc(skip_entries_capacity * sizeof(TpSkipEntry));

	/*
	 * Streaming pass: for each term, stream postings one block at a time
	 * from the N-way merge and write immediately. This avoids loading all
	 * postings for a term into memory at once, which can exceed
	 * MaxAllocSize for high-frequency terms in large corpora.
	 */
	for (i = 0; i < num_terms; i++)
	{
		TpPostingMergeSource *psources;
		int					  num_psources;
		TpBlockPosting		  block_buf[TP_BLOCK_SIZE];
		uint32				  block_count = 0;
		uint32				  doc_count	  = 0;
		uint32				  num_blocks  = 0;

		/* Record where this term's postings start */
		term_blocks[i].posting_offset	= writer.current_offset;
		term_blocks[i].skip_entry_start = skip_entries_count;

		if (terms[i].num_segment_refs == 0)
		{
			term_blocks[i].doc_freq	   = 0;
			term_blocks[i].block_count = 0;
			continue;
		}

		/* Initialize N-way merge sources */
		psources =
				init_term_posting_sources(&terms[i], sources, &num_psources);

		/*
		 * Stream through postings one block at a time. We collect
		 * TP_BLOCK_SIZE postings into block_buf, then write the
		 * block and its skip entry immediately.
		 */
		while (true)
		{
			int min_idx = find_min_posting_source(psources, num_psources);
			if (min_idx < 0)
				break;

			{
				int	   src_idx	  = terms[i].segment_refs[min_idx].segment_idx;
				uint32 old_doc_id = psources[min_idx].current.old_doc_id;
				uint32 new_doc_id =
						doc_mapping.old_to_new[src_idx][old_doc_id];

				block_buf[block_count].doc_id = new_doc_id;
				block_buf[block_count].frequency =
						psources[min_idx].current.frequency;
				block_buf[block_count].fieldnorm =
						psources[min_idx].current.fieldnorm;
				block_buf[block_count].reserved = 0;
			}
			block_count++;
			doc_count++;

			posting_source_advance(&psources[min_idx]);

			/* Write block when full */
			if (block_count == TP_BLOCK_SIZE)
			{
				TpSkipEntry skip;
				uint16		max_tf		= 0;
				uint8		min_norm	= 255;
				uint32		last_doc_id = 0;
				uint32		j;

				for (j = 0; j < block_count; j++)
				{
					if (block_buf[j].doc_id > last_doc_id)
						last_doc_id = block_buf[j].doc_id;
					if (block_buf[j].frequency > max_tf)
						max_tf = block_buf[j].frequency;
					if (block_buf[j].fieldnorm < min_norm)
						min_norm = block_buf[j].fieldnorm;
				}

				skip.last_doc_id	= last_doc_id;
				skip.doc_count		= (uint8)block_count;
				skip.block_max_tf	= max_tf;
				skip.block_max_norm = min_norm;
				skip.posting_offset = writer.current_offset;
				memset(skip.reserved, 0, sizeof(skip.reserved));

				if (tp_compress_segments)
				{
					uint8  cbuf[TP_MAX_COMPRESSED_BLOCK_SIZE];
					uint32 csize;

					csize = tp_compress_block(block_buf, block_count, cbuf);
					skip.flags = TP_BLOCK_FLAG_DELTA;
					tp_segment_writer_write(&writer, cbuf, csize);
				}
				else
				{
					skip.flags = TP_BLOCK_FLAG_UNCOMPRESSED;
					tp_segment_writer_write(
							&writer,
							block_buf,
							block_count * sizeof(TpBlockPosting));
				}

				if (skip_entries_count >= skip_entries_capacity)
				{
					skip_entries_capacity *= 2;
					all_skip_entries = repalloc_huge(
							all_skip_entries,
							skip_entries_capacity * sizeof(TpSkipEntry));
				}
				all_skip_entries[skip_entries_count++] = skip;

				num_blocks++;
				block_count = 0;
			}
		}

		/* Write final partial block if any */
		if (block_count > 0)
		{
			TpSkipEntry skip;
			uint16		max_tf		= 0;
			uint8		min_norm	= 255;
			uint32		last_doc_id = 0;
			uint32		j;

			for (j = 0; j < block_count; j++)
			{
				if (block_buf[j].doc_id > last_doc_id)
					last_doc_id = block_buf[j].doc_id;
				if (block_buf[j].frequency > max_tf)
					max_tf = block_buf[j].frequency;
				if (block_buf[j].fieldnorm < min_norm)
					min_norm = block_buf[j].fieldnorm;
			}

			skip.last_doc_id	= last_doc_id;
			skip.doc_count		= (uint8)block_count;
			skip.block_max_tf	= max_tf;
			skip.block_max_norm = min_norm;
			skip.posting_offset = writer.current_offset;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			if (tp_compress_segments)
			{
				uint8  cbuf[TP_MAX_COMPRESSED_BLOCK_SIZE];
				uint32 csize;

				csize	   = tp_compress_block(block_buf, block_count, cbuf);
				skip.flags = TP_BLOCK_FLAG_DELTA;
				tp_segment_writer_write(&writer, cbuf, csize);
			}
			else
			{
				skip.flags = TP_BLOCK_FLAG_UNCOMPRESSED;
				tp_segment_writer_write(
						&writer,
						block_buf,
						block_count * sizeof(TpBlockPosting));
			}

			if (skip_entries_count >= skip_entries_capacity)
			{
				skip_entries_capacity *= 2;
				all_skip_entries = repalloc_huge(
						all_skip_entries,
						skip_entries_capacity * sizeof(TpSkipEntry));
			}
			all_skip_entries[skip_entries_count++] = skip;

			num_blocks++;
		}

		term_blocks[i].doc_freq	   = doc_count;
		term_blocks[i].block_count = (uint16)num_blocks;

		free_term_posting_sources(psources, num_psources);

		/* Check for interrupt during long merges */
		if ((i % 1000) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	/* Skip index starts here - after all postings */
	header.skip_index_offset = writer.current_offset;

	/* Write all accumulated skip entries */
	if (skip_entries_count > 0)
	{
		tp_segment_writer_write(
				&writer,
				all_skip_entries,
				skip_entries_count * sizeof(TpSkipEntry));
	}

	pfree(all_skip_entries);

	/* Write fieldnorm table */
	header.fieldnorm_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer, docmap->fieldnorms, docmap->num_docs * sizeof(uint8));
	}

	/* Write CTID pages array */
	header.ctid_pages_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_pages,
				docmap->num_docs * sizeof(BlockNumber));
	}

	/* Write CTID offsets array */
	header.ctid_offsets_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_offsets,
				docmap->num_docs * sizeof(OffsetNumber));
	}

	/* Flush and write page index */
	tp_segment_writer_flush(&writer);

	/*
	 * Mark buffer as empty to prevent tp_segment_writer_finish from flushing
	 * again and overwriting our dict entry updates.
	 */
	writer.buffer_pos = SizeOfPageHeaderData;

	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);
	header.page_index = page_index_root;
	header.data_size  = writer.current_offset;
	header.num_pages  = writer.pages_allocated;

	/*
	 * Now write the dictionary entries with correct skip_index_offset values.
	 * We need to update each entry on disk. Do this BEFORE
	 * tp_segment_writer_finish so writer.pages is still valid.
	 */
	{
		Buffer dict_buf = InvalidBuffer;
		uint32 entry_logical_page;
		uint32 current_page = UINT32_MAX;

		for (i = 0; i < num_terms; i++)
		{
			TpDictEntry entry;
			uint64		entry_offset;
			uint32		page_offset;
			BlockNumber physical_block;

			/* Build the entry */
			entry.skip_index_offset =
					header.skip_index_offset +
					((uint64)term_blocks[i].skip_entry_start *
					 sizeof(TpSkipEntry));
			entry.block_count = term_blocks[i].block_count;
			entry.reserved	  = 0;
			entry.doc_freq	  = term_blocks[i].doc_freq;

			/* Calculate where this entry is in the segment */
			entry_offset = header.entries_offset +
						   ((uint64)i * sizeof(TpDictEntry));
			entry_logical_page = (uint32)(entry_offset /
										  SEGMENT_DATA_PER_PAGE);
			page_offset = (uint32)(entry_offset % SEGMENT_DATA_PER_PAGE);

			/* Read page if different from current */
			if (entry_logical_page != current_page)
			{
				if (current_page != UINT32_MAX)
				{
					MarkBufferDirty(dict_buf);
					UnlockReleaseBuffer(dict_buf);
				}

				physical_block = writer.pages[entry_logical_page];
				dict_buf	   = ReadBuffer(index, physical_block);
				LockBuffer(dict_buf, BUFFER_LOCK_EXCLUSIVE);
				current_page = entry_logical_page;
			}

			/* Write entry to page - handle page boundary spanning */
			{
				uint32 bytes_on_this_page = SEGMENT_DATA_PER_PAGE -
											page_offset;

				if (bytes_on_this_page >= sizeof(TpDictEntry))
				{
					/* Entry fits entirely on this page */
					Page  page = BufferGetPage(dict_buf);
					char *dest = (char *)page + SizeOfPageHeaderData +
								 page_offset;
					memcpy(dest, &entry, sizeof(TpDictEntry));
				}
				else
				{
					/* Entry spans two pages */
					Page  page = BufferGetPage(dict_buf);
					char *dest = (char *)page + SizeOfPageHeaderData +
								 page_offset;
					char *src = (char *)&entry;

					/* Write first part to current page */
					memcpy(dest, src, bytes_on_this_page);

					/* Move to next page */
					MarkBufferDirty(dict_buf);
					UnlockReleaseBuffer(dict_buf);

					entry_logical_page++;
					if (entry_logical_page >= writer.pages_allocated)
						ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("dict entry spans beyond allocated")));

					physical_block = writer.pages[entry_logical_page];
					dict_buf	   = ReadBuffer(index, physical_block);
					LockBuffer(dict_buf, BUFFER_LOCK_EXCLUSIVE);
					current_page = entry_logical_page;

					/* Write remaining part to next page */
					page = BufferGetPage(dict_buf);
					dest = (char *)page + SizeOfPageHeaderData;
					memcpy(dest,
						   src + bytes_on_this_page,
						   sizeof(TpDictEntry) - bytes_on_this_page);
				}
			}
		}

		/* Release last buffer */
		if (current_page != UINT32_MAX)
		{
			MarkBufferDirty(dict_buf);
			UnlockReleaseBuffer(dict_buf);
		}
	}

	tp_segment_writer_finish(&writer);

	/* Update header on disk with final offsets */
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page		= BufferGetPage(header_buf);
	existing_header = (TpSegmentHeader *)PageGetContents(header_page);

	existing_header->dictionary_offset	 = header.dictionary_offset;
	existing_header->strings_offset		 = header.strings_offset;
	existing_header->entries_offset		 = header.entries_offset;
	existing_header->postings_offset	 = header.postings_offset;
	existing_header->skip_index_offset	 = header.skip_index_offset;
	existing_header->fieldnorm_offset	 = header.fieldnorm_offset;
	existing_header->ctid_pages_offset	 = header.ctid_pages_offset;
	existing_header->ctid_offsets_offset = header.ctid_offsets_offset;
	existing_header->num_docs			 = header.num_docs;
	existing_header->data_size			 = header.data_size;
	existing_header->num_pages			 = header.num_pages;
	existing_header->page_index			 = header.page_index;

	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	/* Cleanup */
	pfree(string_offsets);
	pfree(term_blocks);
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
			/*
			 * Use HUGE allocations since vocabulary can exceed
			 * MaxAllocSize when merging many large segments.
			 */
			if (merged_terms == NULL)
				merged_terms = palloc_extended(
						merged_capacity * sizeof(TpMergedTerm),
						MCXT_ALLOC_HUGE);
			else
				merged_terms = repalloc_huge(
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

		/*
		 * Record which segments have this term (don't load postings yet).
		 * IMPORTANT: Use current_merged->term (the pstrdup'd copy) for
		 * comparison, NOT min_term. When we advance sources[min_idx],
		 * merge_source_advance() frees sources[min_idx].current_term, which
		 * min_term points to. Using min_term after that would be
		 * use-after-free undefined behavior.
		 */
		for (i = 0; i < num_sources; i++)
		{
			if (sources[i].exhausted)
				continue;

			if (strcmp(sources[i].current_term, current_merged->term) == 0)
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
		seg_page   = BufferGetPage(seg_buf);
		seg_header = (TpSegmentHeader *)PageGetContents(seg_page);
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
