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
#include "merge_internal.h"
#include "pagemapper.h"
#include "segment.h"
#include "segment_io.h"
#include "state/metapage.h"

/* GUC variable for segment compression */
extern bool tp_compress_segments;

/*
 * Types TpMergeSource, TpTermSegmentRef, TpMergedTerm,
 * TpMergePostingInfo, TpPostingMergeSource, TpMergeDocMapping,
 * and MergeTermBlockInfo are defined in merge_internal.h.
 */

/* ----------------------------------------------------------------
 * Merge sink: unified I/O abstraction
 * ----------------------------------------------------------------
 */

/*
 * Seek to an absolute position in a BufFile and write data.
 */
static void
buffile_write_at(BufFile *file, uint64 abs_pos, const void *data, size_t size)
{
	int	  fileno;
	off_t offset;

	tp_buffile_decompose_offset(abs_pos, &fileno, &offset);
	BufFileSeek(file, fileno, offset, SEEK_SET);
	BufFileWrite(file, data, size);
}

void
merge_sink_init_pages(TpMergeSink *sink, Relation index)
{
	memset(sink, 0, sizeof(TpMergeSink));
	sink->is_buffile = false;
	sink->index		 = index;
	tp_segment_writer_init(&sink->writer, index);
	sink->current_offset = sink->writer.current_offset;
}

void
merge_sink_init_pages_parallel(
		TpMergeSink *sink, Relation index, pg_atomic_uint64 *page_counter)
{
	memset(sink, 0, sizeof(TpMergeSink));
	sink->is_buffile = false;
	sink->index		 = index;
	tp_segment_writer_init_parallel(&sink->writer, index, page_counter);
	sink->current_offset = sink->writer.current_offset;
}

void
merge_sink_init_buffile(TpMergeSink *sink, BufFile *file)
{
	int	  fileno;
	off_t file_offset;

	memset(sink, 0, sizeof(TpMergeSink));
	sink->is_buffile = true;
	sink->file		 = file;

	BufFileTell(file, &fileno, &file_offset);
	sink->base_abs		 = tp_buffile_composite_offset(fileno, file_offset);
	sink->current_offset = 0;
}

/*
 * Sequential append to sink.
 */
static void
merge_sink_write(TpMergeSink *sink, const void *data, uint32 size)
{
	if (!sink->is_buffile)
	{
		tp_segment_writer_write(&sink->writer, data, size);
		sink->current_offset = sink->writer.current_offset;
	}
	else
	{
		buffile_write_at(
				sink->file, sink->base_abs + sink->current_offset, data, size);
		sink->current_offset += size;
	}
}

/*
 * Positioned write for backpatching (dict entries, header).
 * For pages backend, reads/writes through buffer manager.
 */
static void
merge_sink_write_at(
		TpMergeSink *sink, uint64 offset, const void *data, uint32 size)
{
	if (sink->is_buffile)
	{
		buffile_write_at(sink->file, sink->base_abs + offset, data, size);
		return;
	}

	/* Pages backend: loop over logical pages */
	{
		const char *src		  = (const char *)data;
		uint32		remaining = size;
		uint64		pos		  = offset;

		while (remaining > 0)
		{
			uint32		logical_pg = tp_logical_page(pos);
			uint32		pg_off	   = tp_page_offset(pos);
			uint32		avail	   = SEGMENT_DATA_PER_PAGE - pg_off;
			uint32		chunk	   = Min(remaining, avail);
			BlockNumber physical_block;
			Buffer		buf;
			Page		page;

			Assert(logical_pg < sink->writer.pages_allocated);
			physical_block = sink->writer.pages[logical_pg];

			buf = ReadBuffer(sink->index, physical_block);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);
			memcpy((char *)page + SizeOfPageHeaderData + pg_off, src, chunk);
			MarkBufferDirty(buf);
			UnlockReleaseBuffer(buf);

			src += chunk;
			pos += chunk;
			remaining -= chunk;
		}
	}
}

/* ----------------------------------------------------------------
 * Merge source operations
 * ----------------------------------------------------------------
 */

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
bool
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
bool
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
 * Initialize a merge source from a pre-opened reader.
 * Same as merge_source_init but takes a TpSegmentReader directly.
 * Does NOT close the reader on failure (caller owns it).
 */
bool
merge_source_init_from_reader(TpMergeSource *source, TpSegmentReader *reader)
{
	TpSegmentHeader *header;
	TpDictionary	 dict_header;

	memset(source, 0, sizeof(TpMergeSource));
	source->exhausted = true; /* Assume failure */

	if (!reader)
		return false;

	source->reader = reader;
	header		   = reader->header;

	if (header->num_terms == 0)
		return false;

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

	/* Position before first term */
	source->current_idx	 = UINT32_MAX;
	source->exhausted	 = false;
	source->current_term = NULL;

	/* Advance to first term */
	if (!merge_source_advance(source))
	{
		pfree(source->string_offsets);
		source->string_offsets = NULL;
		source->reader		   = NULL; /* Don't close, caller owns it */
		return false;
	}

	return true;
}

/*
 * Close and cleanup a merge source.
 */
void
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
int
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
void
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
	tp_segment_read_skip_entry(
			ps->reader,
			ps->skip_index_offset,
			ps->current_block,
			&ps->skip_entry);

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
void
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
 * Initialize a posting merge source for fast streaming (disjoint mode).
 * Skips posting_source_convert_current since CTID lookups are not needed.
 */
void
posting_source_init_fast(
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
		if (!posting_source_load_block(ps))
			ps->exhausted = true;
	}
}

/*
 * Free posting merge source resources.
 */
void
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
bool
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
 * Advance a posting merge source without CTID conversion (disjoint mode).
 * Reads doc_id, frequency, fieldnorm directly from the block posting
 * array, skipping the expensive CTID lookups in convert_current.
 */
bool
posting_source_advance_fast(TpPostingMergeSource *ps)
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

	return true;
}

/*
 * Find the posting source with the smallest current CTID.
 * Returns -1 if all sources are exhausted.
 */
int
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

/* TpMergeDocMapping is defined in merge_internal.h */

/*
 * Per-source state for the streaming N-way merge of docmaps.
 */
typedef struct TpDocmapMergeSource
{
	BlockNumber	 *ctid_pages;	/* CTID page numbers (4 bytes/doc) */
	OffsetNumber *ctid_offsets; /* CTID tuple offsets (2 bytes/doc) */
	uint8		 *fieldnorms;	/* Encoded fieldnorms (1 byte/doc) */
	uint32		  num_docs;		/* Total docs in this source */
	uint32		  cursor;		/* Current position in arrays */
	bool		  owns_arrays;	/* True if we allocated the arrays */
} TpDocmapMergeSource;

/*
 * Build merged docmap using streaming N-way merge of sorted CTID arrays.
 * Also builds direct mapping arrays for fast old->new doc_id lookup.
 *
 * Each source segment maintains the invariant that doc_ids are in CTID
 * order. An N-way merge of these sorted streams produces the global
 * CTID order without needing a hash table, reducing memory from ~5.5GB
 * to ~2.5GB for 138M documents across 24 segments.
 */
TpDocMapBuilder *
build_merged_docmap(
		TpMergeSource	  *sources,
		int				   num_sources,
		TpMergeDocMapping *mapping,
		bool			   disjoint_sources)
{
	TpDocMapBuilder		*docmap;
	TpDocmapMergeSource *msources;
	uint32				 total_docs = 0;
	BlockNumber			*out_pages;
	OffsetNumber		*out_offsets;
	uint8				*out_fieldnorms;
	uint32				 new_doc_id = 0;
	int					 i;

	/* Initialize mapping arrays */
	mapping->num_sources = num_sources;
	mapping->old_to_new	 = (uint32 **)palloc0(num_sources * sizeof(uint32 *));

	/*
	 * Step 1: Load source CTID and fieldnorm arrays.
	 * Reuse the reader's cached arrays when available to avoid
	 * redundant copies.
	 */
	msources = palloc0(num_sources * sizeof(TpDocmapMergeSource));
	for (i = 0; i < num_sources; i++)
	{
		TpSegmentHeader		*header = sources[i].reader->header;
		TpDocmapMergeSource *ms		= &msources[i];

		ms->num_docs = header->num_docs;
		ms->cursor	 = 0;

		if (ms->num_docs == 0 || header->ctid_pages_offset == 0)
		{
			ms->num_docs = 0; /* Exclude from N-way merge */
			continue;
		}

		total_docs += ms->num_docs;
		mapping->old_to_new[i] = palloc(ms->num_docs * sizeof(uint32));

		/* CTID arrays: use cached if available, else bulk-read */
		if (sources[i].reader->cached_ctid_pages != NULL)
		{
			ms->ctid_pages	 = sources[i].reader->cached_ctid_pages;
			ms->ctid_offsets = sources[i].reader->cached_ctid_offsets;
			ms->owns_arrays	 = false;
		}
		else
		{
			ms->ctid_pages = palloc(ms->num_docs * sizeof(BlockNumber));
			tp_segment_read(
					sources[i].reader,
					header->ctid_pages_offset,
					ms->ctid_pages,
					ms->num_docs * sizeof(BlockNumber));

			ms->ctid_offsets = palloc(ms->num_docs * sizeof(OffsetNumber));
			tp_segment_read(
					sources[i].reader,
					header->ctid_offsets_offset,
					ms->ctid_offsets,
					ms->num_docs * sizeof(OffsetNumber));
			ms->owns_arrays = true;
		}

		/* Fieldnorms: always bulk-read (not cached by reader) */
		ms->fieldnorms = palloc(ms->num_docs * sizeof(uint8));
		tp_segment_read(
				sources[i].reader,
				header->fieldnorm_offset,
				ms->fieldnorms,
				ms->num_docs * sizeof(uint8));
	}

	/* Step 2: Allocate output arrays */
	if (total_docs > 0)
	{
		out_pages	   = palloc(total_docs * sizeof(BlockNumber));
		out_offsets	   = palloc(total_docs * sizeof(OffsetNumber));
		out_fieldnorms = palloc(total_docs * sizeof(uint8));
	}
	else
	{
		out_pages	   = NULL;
		out_offsets	   = NULL;
		out_fieldnorms = NULL;
	}

	/*
	 * Step 3: Merge docmaps.
	 *
	 * When disjoint_sources is true, sources have non-overlapping
	 * CTID ranges in source order, so we concatenate sequentially
	 * instead of doing an N-way comparison. This eliminates the
	 * per-doc CTID comparison overhead.
	 */
	if (disjoint_sources)
	{
		for (i = 0; i < num_sources; i++)
		{
			TpDocmapMergeSource *ms = &msources[i];
			uint32				 j;

			for (j = 0; j < ms->num_docs; j++)
			{
				mapping->old_to_new[i][j]  = new_doc_id;
				out_pages[new_doc_id]	   = ms->ctid_pages[j];
				out_offsets[new_doc_id]	   = ms->ctid_offsets[j];
				out_fieldnorms[new_doc_id] = ms->fieldnorms[j];
				new_doc_id++;
			}
		}
	}
	else
	{
		/*
		 * N-way merge: each source's docs are already in CTID
		 * order. Find the source with smallest current CTID via
		 * linear scan (N is small, typically <= 24).
		 */
		while (new_doc_id < total_docs)
		{
			int			 min_src = -1;
			BlockNumber	 min_page;
			OffsetNumber min_offset;

			for (i = 0; i < num_sources; i++)
			{
				TpDocmapMergeSource *ms = &msources[i];
				BlockNumber			 pg;
				OffsetNumber		 off;

				if (ms->cursor >= ms->num_docs)
					continue;

				pg	= ms->ctid_pages[ms->cursor];
				off = ms->ctid_offsets[ms->cursor];

				if (min_src < 0 || pg < min_page ||
					(pg == min_page && off < min_offset))
				{
					min_src	   = i;
					min_page   = pg;
					min_offset = off;
				}
			}

			Assert(min_src >= 0);

			{
				TpDocmapMergeSource *ms	 = &msources[min_src];
				uint32				 pos = ms->cursor;

				mapping->old_to_new[min_src][pos] = new_doc_id;
				out_pages[new_doc_id]			  = ms->ctid_pages[pos];
				out_offsets[new_doc_id]			  = ms->ctid_offsets[pos];
				out_fieldnorms[new_doc_id]		  = ms->fieldnorms[pos];
				ms->cursor++;
			}
			new_doc_id++;
		}
	}

	/* Step 4: Package into a TpDocMapBuilder (finalized, no hash table) */
	docmap				 = palloc0(sizeof(TpDocMapBuilder));
	docmap->ctid_to_id	 = NULL; /* No hash table needed */
	docmap->num_docs	 = total_docs;
	docmap->capacity	 = total_docs;
	docmap->finalized	 = true;
	docmap->ctid_pages	 = out_pages;
	docmap->ctid_offsets = out_offsets;
	docmap->fieldnorms	 = out_fieldnorms;

	/* Free per-source arrays we allocated */
	for (i = 0; i < num_sources; i++)
	{
		if (msources[i].owns_arrays)
		{
			pfree(msources[i].ctid_pages);
			pfree(msources[i].ctid_offsets);
		}
		if (msources[i].fieldnorms)
			pfree(msources[i].fieldnorms);
	}
	pfree(msources);

	return docmap;
}

/*
 * Free merge doc mapping arrays.
 */
void
free_merge_doc_mapping(TpMergeDocMapping *mapping)
{
	int i;

	for (i = 0; i < mapping->num_sources; i++)
	{
		if (mapping->old_to_new[i])
			pfree(mapping->old_to_new[i]);
	}
	pfree((void *)mapping->old_to_new);
}

/*
 * Initialize N-way merge posting sources for a term.
 * Returns the number of sources initialized.
 */
TpPostingMergeSource *
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
 * Initialize posting sources in fast/disjoint mode (no CTID conversion).
 */
TpPostingMergeSource *
init_term_posting_sources_fast(
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

		posting_source_init_fast(&psources[i], source->reader, &ref->entry);
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

/* MergeTermBlockInfo is defined in merge_internal.h */

/* ----------------------------------------------------------------
 * Unified merged segment writer
 * ----------------------------------------------------------------
 */

/*
 * Write a merged segment to a sink (pages or BufFile).
 *
 * Layout: [header] -> [dictionary] -> [postings] -> [skip index] ->
 *         [fieldnorm] -> [ctid map]
 *
 * For pages sink: also writes page index and backpatches header with
 * num_pages/page_index. Caller reads sink->writer.pages[0] for root.
 *
 * For BufFile sink: caller reads sink->current_offset for data_size.
 */
void
write_merged_segment_to_sink(
		TpMergeSink	  *sink,
		TpMergedTerm  *terms,
		uint32		   num_terms,
		TpMergeSource *sources,
		int			   num_sources,
		uint32		   target_level,
		uint64		   total_tokens,
		bool		   disjoint_sources)
{
	TpSegmentHeader		header;
	TpDictionary		dict;
	TpDocMapBuilder	   *docmap;
	TpMergeDocMapping	doc_mapping;
	MergeTermBlockInfo *term_blocks;
	uint32			   *string_offsets;
	uint32				string_pos;
	uint32				i;

	/* Accumulated skip entries for all terms */
	TpSkipEntry *all_skip_entries;
	uint32		 skip_entries_count;
	uint32		 skip_entries_capacity;

	if (num_terms == 0)
		return;

	/* Build docmap and direct mapping arrays from source segments */
	docmap = build_merged_docmap(
			sources, num_sources, &doc_mapping, disjoint_sources);

	/*
	 * For BufFile sink, docmap reads may have moved the cursor.
	 * Re-seek to our recorded base position.
	 */
	if (sink->is_buffile)
	{
		int	  fileno;
		off_t offset;

		tp_buffile_decompose_offset(sink->base_abs, &fileno, &offset);
		BufFileSeek(sink->file, fileno, offset, SEEK_SET);
		sink->current_offset = 0;
	}

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
	header.page_index	= InvalidBlockNumber;

	/* Write placeholder header */
	merge_sink_write(sink, &header, sizeof(TpSegmentHeader));

	/* Dictionary immediately follows header */
	header.dictionary_offset = sink->current_offset;

	/* Write dictionary header */
	memset(&dict, 0, sizeof(dict));
	dict.num_terms = num_terms;
	merge_sink_write(sink, &dict, offsetof(TpDictionary, string_offsets));

	/* Calculate string offsets */
	string_offsets = palloc0(num_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + terms[i].term_len + sizeof(uint32);
	}

	/* Write string offsets array */
	merge_sink_write(sink, string_offsets, num_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = sink->current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		merge_sink_write(sink, &length, sizeof(uint32));
		merge_sink_write(sink, terms[i].term, length);
		merge_sink_write(sink, &dict_offset, sizeof(uint32));
	}

	/* Record entries offset - dict entries written after postings */
	header.entries_offset = sink->current_offset;

	/* Write placeholder dict entries */
	{
		TpDictEntry placeholder;
		memset(&placeholder, 0, sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
			merge_sink_write(sink, &placeholder, sizeof(TpDictEntry));
	}

	/* Postings start here */
	header.postings_offset = sink->current_offset;

	/* Initialize per-term tracking and skip entry accumulator */
	term_blocks = palloc0(num_terms * sizeof(MergeTermBlockInfo));

	skip_entries_capacity = 1024;
	skip_entries_count	  = 0;
	all_skip_entries = palloc(skip_entries_capacity * sizeof(TpSkipEntry));

	/*
	 * Helper macro: flush a full or partial block_buf to the sink.
	 * Computes skip entry, optionally compresses, writes data,
	 * and accumulates the skip entry.
	 */
#define FLUSH_BLOCK(block_buf, block_count, num_blocks)                         \
	do                                                                          \
	{                                                                           \
		TpSkipEntry skip_;                                                      \
		uint16		max_tf_	  = 0;                                              \
		uint8		min_norm_ = 255;                                            \
		uint32		last_did_ = 0;                                              \
		uint32		j_;                                                         \
                                                                                \
		for (j_ = 0; j_ < (block_count); j_++)                                  \
		{                                                                       \
			if ((block_buf)[j_].doc_id > last_did_)                             \
				last_did_ = (block_buf)[j_].doc_id;                             \
			if ((block_buf)[j_].frequency > max_tf_)                            \
				max_tf_ = (block_buf)[j_].frequency;                            \
			if ((block_buf)[j_].fieldnorm < min_norm_)                          \
				min_norm_ = (block_buf)[j_].fieldnorm;                          \
		}                                                                       \
                                                                                \
		skip_.last_doc_id	 = last_did_;                                       \
		skip_.doc_count		 = (uint8)(block_count);                            \
		skip_.block_max_tf	 = max_tf_;                                         \
		skip_.block_max_norm = min_norm_;                                       \
		skip_.posting_offset = sink->current_offset;                            \
		memset(skip_.reserved, 0, sizeof(skip_.reserved));                      \
                                                                                \
		if (tp_compress_segments)                                               \
		{                                                                       \
			uint8  cbuf_[TP_MAX_COMPRESSED_BLOCK_SIZE];                         \
			uint32 csize_;                                                      \
                                                                                \
			csize_		= tp_compress_block((block_buf), (block_count), cbuf_); \
			skip_.flags = TP_BLOCK_FLAG_DELTA;                                  \
			merge_sink_write(sink, cbuf_, csize_);                              \
		}                                                                       \
		else                                                                    \
		{                                                                       \
			skip_.flags = TP_BLOCK_FLAG_UNCOMPRESSED;                           \
			merge_sink_write(                                                   \
					sink,                                                       \
					(block_buf),                                                \
					(block_count) * sizeof(TpBlockPosting));                    \
		}                                                                       \
                                                                                \
		if (skip_entries_count >= skip_entries_capacity)                        \
		{                                                                       \
			skip_entries_capacity *= 2;                                         \
			all_skip_entries = repalloc_huge(                                   \
					all_skip_entries,                                           \
					skip_entries_capacity * sizeof(TpSkipEntry));               \
		}                                                                       \
		all_skip_entries[skip_entries_count++] = skip_;                         \
		(num_blocks)++;                                                         \
	} while (0)

	/*
	 * Streaming pass: for each term, stream postings one block at a
	 * time and write immediately.
	 *
	 * When disjoint_sources is true, drain sources sequentially
	 * (source 0 fully, then source 1, etc.) without CTID lookups.
	 * Otherwise, use N-way CTID-comparison merge.
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
		term_blocks[i].posting_offset	= sink->current_offset;
		term_blocks[i].skip_entry_start = skip_entries_count;

		if (terms[i].num_segment_refs == 0)
		{
			term_blocks[i].doc_freq	   = 0;
			term_blocks[i].block_count = 0;
			continue;
		}

		if (disjoint_sources)
		{
			/*
			 * Fast path: sequential drain. Sources have disjoint
			 * CTID ranges, so drain source 0, then 1, etc.
			 * Reads doc_id/frequency/fieldnorm directly from
			 * the block posting array, skipping CTID lookups.
			 */
			psources = init_term_posting_sources_fast(
					&terms[i], sources, &num_psources);

			for (int src = 0; src < num_psources; src++)
			{
				while (!psources[src].exhausted)
				{
					TpBlockPosting *bp =
							&psources[src].block_postings
									 [psources[src].current_in_block];
					int	   seg_idx = terms[i].segment_refs[src].segment_idx;
					uint32 new_doc_id =
							doc_mapping.old_to_new[seg_idx][bp->doc_id];

					block_buf[block_count].doc_id	 = new_doc_id;
					block_buf[block_count].frequency = bp->frequency;
					block_buf[block_count].fieldnorm = bp->fieldnorm;
					block_buf[block_count].reserved	 = 0;
					block_count++;
					doc_count++;

					posting_source_advance_fast(&psources[src]);

					if (block_count == TP_BLOCK_SIZE)
					{
						FLUSH_BLOCK(block_buf, block_count, num_blocks);
						block_count = 0;
					}
				}
			}
		}
		else
		{
			/*
			 * Standard N-way merge: compare CTIDs across sources.
			 */
			psources = init_term_posting_sources(
					&terms[i], sources, &num_psources);

			while (true)
			{
				int min_idx = find_min_posting_source(psources, num_psources);
				if (min_idx < 0)
					break;

				{
					int src_idx = terms[i].segment_refs[min_idx].segment_idx;
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

				if (block_count == TP_BLOCK_SIZE)
				{
					FLUSH_BLOCK(block_buf, block_count, num_blocks);
					block_count = 0;
				}
			}
		}

		/* Write final partial block if any */
		if (block_count > 0)
			FLUSH_BLOCK(block_buf, block_count, num_blocks);

		term_blocks[i].doc_freq	   = doc_count;
		term_blocks[i].block_count = (uint16)num_blocks;

		free_term_posting_sources(psources, num_psources);

		/* Check for interrupt during long merges */
		if ((i % 1000) == 0)
			CHECK_FOR_INTERRUPTS();
	}

#undef FLUSH_BLOCK

	/* Skip index starts here - after all postings */
	header.skip_index_offset = sink->current_offset;

	/* Write all accumulated skip entries */
	if (skip_entries_count > 0)
	{
		merge_sink_write(
				sink,
				all_skip_entries,
				skip_entries_count * sizeof(TpSkipEntry));
	}

	pfree(all_skip_entries);

	/* Write fieldnorm table */
	header.fieldnorm_offset = sink->current_offset;
	if (docmap->num_docs > 0)
	{
		merge_sink_write(
				sink, docmap->fieldnorms, docmap->num_docs * sizeof(uint8));
	}

	/* Write CTID pages array */
	header.ctid_pages_offset = sink->current_offset;
	if (docmap->num_docs > 0)
	{
		merge_sink_write(
				sink,
				docmap->ctid_pages,
				docmap->num_docs * sizeof(BlockNumber));
	}

	/* Write CTID offsets array */
	header.ctid_offsets_offset = sink->current_offset;
	if (docmap->num_docs > 0)
	{
		merge_sink_write(
				sink,
				docmap->ctid_offsets,
				docmap->num_docs * sizeof(OffsetNumber));
	}

	/* Finalize data_size */
	header.data_size = sink->current_offset;

	/* Pages-only: flush writer, write page index */
	if (!sink->is_buffile)
	{
		BlockNumber page_index_root;

		tp_segment_writer_flush(&sink->writer);
		sink->writer.buffer_pos = SizeOfPageHeaderData;

		if (sink->writer.page_counter != NULL)
			page_index_root = write_page_index_with_counter(
					sink->index,
					sink->writer.pages,
					sink->writer.pages_allocated,
					sink->writer.page_counter);
		else
			page_index_root = write_page_index(
					sink->index,
					sink->writer.pages,
					sink->writer.pages_allocated);
		header.page_index = page_index_root;
		header.num_pages  = sink->writer.pages_allocated;
	}

	/* Backpatch dict entries */
	{
		TpDictEntry *dict_entries;

		dict_entries = palloc(num_terms * sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
		{
			dict_entries[i].skip_index_offset =
					header.skip_index_offset +
					((uint64)term_blocks[i].skip_entry_start *
					 sizeof(TpSkipEntry));
			dict_entries[i].block_count = term_blocks[i].block_count;
			dict_entries[i].reserved	= 0;
			dict_entries[i].doc_freq	= term_blocks[i].doc_freq;
		}

		merge_sink_write_at(
				sink,
				header.entries_offset,
				dict_entries,
				num_terms * sizeof(TpDictEntry));
		pfree(dict_entries);
	}

	/* Backpatch header */
	merge_sink_write_at(sink, 0, &header, sizeof(TpSegmentHeader));

	/* Pages-only: finish writer */
	if (!sink->is_buffile)
		tp_segment_writer_finish(&sink->writer);

	/* BufFile: seek to end position */
	if (sink->is_buffile)
	{
		int	  end_fileno;
		off_t end_offset;

		tp_buffile_decompose_offset(
				sink->base_abs + sink->current_offset,
				&end_fileno,
				&end_offset);
		BufFileSeek(sink->file, end_fileno, end_offset, SEEK_SET);
	}

	/* Cleanup */
	pfree(string_offsets);
	pfree(term_blocks);
	free_merge_doc_mapping(&doc_mapping);
	tp_docmap_destroy(docmap);
}

/* ----------------------------------------------------------------
 * Level-based merge (uses pages sink)
 * ----------------------------------------------------------------
 */

/*
 * Merge up to max_merge segments from the specified level into a
 * single segment at level+1.  Returns the new segment's header
 * block, or InvalidBlockNumber on failure.
 */
BlockNumber
tp_merge_level_segments(Relation index, uint32 level, uint32 max_merge)
{
	TpIndexMetaPage metap;
	Buffer			metabuf;
	Page			metapage;
	BlockNumber		first_segment;
	uint32			total_at_level;
	uint32			segment_count;
	TpMergeSource  *sources;
	int				num_sources;
	int				i;
	BlockNumber		current;
	BlockNumber		remainder_head; /* First unmerged segment */
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

	first_segment  = metap->level_heads[level];
	total_at_level = metap->level_counts[level];

	UnlockReleaseBuffer(metabuf);

	if (first_segment == InvalidBlockNumber || total_at_level == 0)
	{
		return InvalidBlockNumber;
	}

	/*
	 * Merge at most max_merge segments per batch.  For normal
	 * compaction this is segments_per_level; for post-build
	 * compaction it can be UINT32_MAX to merge everything.
	 */
	segment_count = Min(total_at_level, max_merge);

	elog(DEBUG1,
		 "Merging %u of %u segments at level %u",
		 segment_count,
		 total_at_level,
		 level);

	/*
	 * Allocate page tracking arrays in current context (not merge context)
	 * so they survive the merge context deletion.
	 */
	segment_pages = (BlockNumber **)palloc0(
			sizeof(BlockNumber *) * segment_count);
	segment_page_counts = palloc0(sizeof(uint32) * segment_count);

	/* Create memory context for merge operation */
	merge_ctx = AllocSetContextCreate(
			CurrentMemoryContext, "Segment Merge", ALLOCSET_DEFAULT_SIZES);
	old_ctx = MemoryContextSwitchTo(merge_ctx);

	/* Allocate array for merge sources */
	sources		= palloc0(sizeof(TpMergeSource) * segment_count);
	num_sources = 0;

	/*
	 * Walk the chain, consuming exactly segment_count segments.
	 * Track segments_walked separately from num_sources because
	 * merge_source_init can fail (e.g. empty segment), in which
	 * case we still consumed the segment from the chain.
	 */
	{
		uint32 segments_walked = 0;

		current = first_segment;
		while (current != InvalidBlockNumber &&
			   segments_walked < segment_count)
		{
			TpSegmentReader *reader;
			BlockNumber		 next;
			uint64			 seg_tokens;

			reader = tp_segment_open(index, current);
			if (reader)
			{
				next	   = reader->header->next_segment;
				seg_tokens = reader->header->total_tokens;
				tp_segment_close(reader);

				/*
				 * Collect pages for later freeing (parent context).
				 */
				{
					MemoryContext save_ctx = MemoryContextSwitchTo(old_ctx);
					uint32		  page_count;

					page_count = tp_segment_collect_pages(
							index,
							current,
							&segment_pages[num_segments_tracked]);
					segment_page_counts[num_segments_tracked] = page_count;
					total_pages_to_free += page_count;
					num_segments_tracked++;

					MemoryContextSwitchTo(save_ctx);
				}

				if (merge_source_init(&sources[num_sources], index, current))
				{
					total_tokens += seg_tokens;
					num_sources++;
				}

				current = next;
			}
			else
			{
				break;
			}
			segments_walked++;
		}

		/* Update segment_count to reflect actual segments consumed */
		segment_count = segments_walked;
	}

	/* First unmerged segment (may be InvalidBlockNumber) */
	remainder_head = current;

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
		pfree((void *)segment_pages);
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

		/* Grow merged terms array if needed (may exceed 1GB for large
		 * corpora) */
		if (num_merged_terms >= merged_capacity)
		{
			merged_capacity = merged_capacity == 0 ? 1024
												   : merged_capacity * 2;
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

	/* Write merged segment using pages sink */
	if (num_merged_terms > 0)
	{
		TpMergeSink sink;

		merge_sink_init_pages(&sink, index);
		new_segment = sink.writer.pages[0];

		write_merged_segment_to_sink(
				&sink,
				merged_terms,
				num_merged_terms,
				sources,
				num_sources,
				level + 1,
				total_tokens,
				false);

		/* Free writer pages array */
		if (sink.writer.pages)
			pfree(sink.writer.pages);

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

	/* Close all sources (after write is done with them) */
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
		pfree((void *)segment_pages);
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

	/*
	 * Update source level: keep any unmerged remainder segments.
	 * If we merged all segments, the level is now empty.
	 */
	metap->level_heads[level]  = remainder_head;
	metap->level_counts[level] = total_at_level - segment_count;

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
	pfree((void *)segment_pages);
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

	/*
	 * Merge batches of segments_per_level until the level is
	 * below threshold.  Each batch produces one segment at
	 * level+1; after the loop we check if that level also needs
	 * compaction.
	 */
	while (level_count >= (uint16)tp_segments_per_level)
	{
		if (tp_merge_level_segments(
					index, level, (uint32)tp_segments_per_level) ==
			InvalidBlockNumber)
			break;

		/* Re-read the level count after merge */
		metabuf = ReadBuffer(index, 0);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		metapage	= BufferGetPage(metabuf);
		metap		= (TpIndexMetaPage)PageGetContents(metapage);
		level_count = metap->level_counts[level];
		UnlockReleaseBuffer(metabuf);
	}

	/* Check if next level now needs compaction */
	tp_maybe_compact_level(index, level + 1);
}

/*
 * Force-merge all segments into a single segment, à la Lucene's
 * forceMerge(1).  Merges ALL segments at each level in a single
 * batch, ignoring the segments_per_level threshold.
 */
void
tp_force_merge_all(Relation index)
{
	for (uint32 level = 0; level < TP_MAX_LEVELS - 1; level++)
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;
		uint16			count;

		metabuf = ReadBuffer(index, 0);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);
		count	 = metap->level_counts[level];
		UnlockReleaseBuffer(metabuf);

		if (count < 2)
			break; /* Nothing to merge at this level or above */

		tp_merge_level_segments(index, level, UINT32_MAX);
	}
}
