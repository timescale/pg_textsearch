/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.c - Two-phase parallel index build
 *
 * Architecture:
 * - Phase 1: Each worker scans a heap partition, builds a local
 *   TpBuildContext, flushes segments to BufFile, and compacts
 *   within BufFile. Workers report total_pages_needed.
 * - Barrier: Leader sums pages, pre-extends relation with
 *   ExtendBufferedRelBy, sets atomic next_page counter.
 * - Phase 2: Workers reopen their BufFile read-only and write
 *   segments directly to pre-allocated index pages using atomic
 *   page allocation. Workers report seg_roots[] to shared memory.
 * - Leader reads seg_roots from workers (no BufFile I/O), chains
 *   segments into level lists, updates metapage, runs compaction.
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <commands/progress.h>
#include <inttypes.h>
#include <miscadmin.h>
#include <storage/buffile.h>
#include <storage/bufmgr.h>
#include <storage/condition_variable.h>
#include <storage/smgr.h>
#include <tsearch/ts_type.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/wait_event.h>

#include "am.h"
#include "build_context.h"
#include "build_parallel.h"
#include "constants.h"
#include "segment/compression.h"
#include "segment/docmap.h"
#include "segment/fieldnorm.h"
#include "segment/merge.h"
#include "segment/merge_internal.h"
#include "segment/pagemapper.h"
#include "segment/segment.h"
#include "state/metapage.h"

/* Defined in build.c, extracts terms from TSVector */
extern int tp_extract_terms_from_tsvector(
		TSVector tsvector,
		char  ***terms_out,
		int32  **frequencies_out,
		int		*term_count_out);

/* GUC variable for segment compression */
extern bool tp_compress_segments;

/* Chunk size for streaming temp segments to index pages */
#define TP_COPY_BUF_SIZE (64 * 1024)

/* ----------------------------------------------------------------
 * Worker-local segment tracking
 * ----------------------------------------------------------------
 */

typedef struct WorkerSegmentEntry
{
	uint64 offset;	  /* BufFile offset */
	uint64 data_size; /* Segment bytes */
	uint32 level;	  /* 0=L0, 1=L1, etc. */
	bool   consumed;  /* merged into higher level */
} WorkerSegmentEntry;

typedef struct WorkerSegmentTracker
{
	WorkerSegmentEntry *entries;
	uint32				count;
	uint32				capacity;
	uint32				level_counts[TP_MAX_LEVELS];
} WorkerSegmentTracker;

static void
tracker_init(WorkerSegmentTracker *tracker)
{
	tracker->capacity = 32;
	tracker->count	  = 0;
	tracker->entries  = palloc(tracker->capacity * sizeof(WorkerSegmentEntry));
	memset(tracker->level_counts, 0, sizeof(tracker->level_counts));
}

static void
tracker_add_segment(
		WorkerSegmentTracker *tracker,
		uint64				  offset,
		uint64				  data_size,
		uint32				  level)
{
	if (tracker->count >= tracker->capacity)
	{
		tracker->capacity *= 2;
		tracker->entries = repalloc(
				tracker->entries,
				tracker->capacity * sizeof(WorkerSegmentEntry));
	}
	tracker->entries[tracker->count].offset	   = offset;
	tracker->entries[tracker->count].data_size = data_size;
	tracker->entries[tracker->count].level	   = level;
	tracker->entries[tracker->count].consumed  = false;
	tracker->count++;
	tracker->level_counts[level]++;
}

static void
tracker_destroy(WorkerSegmentTracker *tracker)
{
	if (tracker->entries)
		pfree(tracker->entries);
}

/* ----------------------------------------------------------------
 * BufFile-based merged segment writer
 *
 * Combines N segments from BufFile into a single output segment
 * written back to BufFile. Structurally identical to
 * write_merged_segment() in merge.c but uses BufFile I/O.
 * ----------------------------------------------------------------
 */
static uint64
write_merged_segment_to_buffile(
		BufFile		  *file,
		TpMergedTerm  *terms,
		uint32		   num_terms,
		TpMergeSource *sources,
		int			   num_sources,
		uint32		   target_level,
		uint64		   total_tokens)
{
	TpSegmentHeader		header;
	TpDictionary		dict;
	TpDocMapBuilder	   *docmap;
	TpMergeDocMapping	doc_mapping;
	MergeTermBlockInfo *term_blocks;
	uint32			   *string_offsets;
	uint32				string_pos;
	uint32				i;

	int	  base_fileno;
	off_t base_file_offset;

	uint64 current_offset;

	/* Accumulated skip entries for all terms */
	TpSkipEntry *all_skip_entries;
	uint32		 skip_entries_count;
	uint32		 skip_entries_capacity;

	if (num_terms == 0)
		return 0;

	/* Build docmap and direct mapping arrays */
	docmap = build_merged_docmap(sources, num_sources, &doc_mapping);

	/* Record starting position for later seek-back */
	BufFileTell(file, &base_fileno, &base_file_offset);

	current_offset = 0;

	/* Initialize header */
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

	/* Dictionary immediately follows header */
	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Write placeholder header */
	BufFileWrite(file, &header, sizeof(TpSegmentHeader));
	current_offset += sizeof(TpSegmentHeader);

	/* Write dictionary header */
	memset(&dict, 0, sizeof(dict));
	dict.num_terms = num_terms;
	BufFileWrite(file, &dict, offsetof(TpDictionary, string_offsets));
	current_offset += offsetof(TpDictionary, string_offsets);

	/* Calculate and write string offsets */
	string_offsets = palloc0(num_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + terms[i].term_len + sizeof(uint32);
	}
	BufFileWrite(file, string_offsets, num_terms * sizeof(uint32));
	current_offset += num_terms * sizeof(uint32);

	/* Write string pool */
	header.strings_offset = current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		BufFileWrite(file, &length, sizeof(uint32));
		BufFileWrite(file, terms[i].term, length);
		BufFileWrite(file, &dict_offset, sizeof(uint32));
		current_offset += sizeof(uint32) + length + sizeof(uint32);
	}

	/* Record entries offset */
	header.entries_offset = current_offset;

	/* Write placeholder dict entries */
	{
		TpDictEntry placeholder;

		memset(&placeholder, 0, sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
		{
			BufFileWrite(file, &placeholder, sizeof(TpDictEntry));
			current_offset += sizeof(TpDictEntry);
		}
	}

	/* Postings start here */
	header.postings_offset = current_offset;

	/* Initialize per-term tracking and skip entry accumulator */
	term_blocks = palloc0(num_terms * sizeof(MergeTermBlockInfo));

	skip_entries_capacity = 1024;
	skip_entries_count	  = 0;
	all_skip_entries = palloc(skip_entries_capacity * sizeof(TpSkipEntry));

	/*
	 * Streaming posting merge: for each term, N-way merge postings
	 * from source segments and write to BufFile.
	 */
	for (i = 0; i < num_terms; i++)
	{
		TpPostingMergeSource *psources;
		int					  num_psources;
		TpBlockPosting		  block_buf[TP_BLOCK_SIZE];
		uint32				  block_count = 0;
		uint32				  doc_count	  = 0;
		uint32				  num_blocks  = 0;

		term_blocks[i].posting_offset	= current_offset;
		term_blocks[i].skip_entry_start = skip_entries_count;

		if (terms[i].num_segment_refs == 0)
		{
			term_blocks[i].doc_freq	   = 0;
			term_blocks[i].block_count = 0;
			continue;
		}

		psources =
				init_term_posting_sources(&terms[i], sources, &num_psources);

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
				skip.posting_offset = current_offset;
				memset(skip.reserved, 0, sizeof(skip.reserved));

				if (tp_compress_segments)
				{
					uint8  cbuf[TP_MAX_COMPRESSED_BLOCK_SIZE];
					uint32 csize;

					csize = tp_compress_block(block_buf, block_count, cbuf);
					skip.flags = TP_BLOCK_FLAG_DELTA;
					BufFileWrite(file, cbuf, csize);
					current_offset += csize;
				}
				else
				{
					skip.flags = TP_BLOCK_FLAG_UNCOMPRESSED;
					BufFileWrite(
							file,
							block_buf,
							block_count * sizeof(TpBlockPosting));
					current_offset += block_count * sizeof(TpBlockPosting);
				}

				if (skip_entries_count >= skip_entries_capacity)
				{
					skip_entries_capacity *= 2;
					all_skip_entries = repalloc(
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
			skip.posting_offset = current_offset;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			if (tp_compress_segments)
			{
				uint8  cbuf[TP_MAX_COMPRESSED_BLOCK_SIZE];
				uint32 csize;

				csize	   = tp_compress_block(block_buf, block_count, cbuf);
				skip.flags = TP_BLOCK_FLAG_DELTA;
				BufFileWrite(file, cbuf, csize);
				current_offset += csize;
			}
			else
			{
				skip.flags = TP_BLOCK_FLAG_UNCOMPRESSED;
				BufFileWrite(
						file, block_buf, block_count * sizeof(TpBlockPosting));
				current_offset += block_count * sizeof(TpBlockPosting);
			}

			if (skip_entries_count >= skip_entries_capacity)
			{
				skip_entries_capacity *= 2;
				all_skip_entries = repalloc(
						all_skip_entries,
						skip_entries_capacity * sizeof(TpSkipEntry));
			}
			all_skip_entries[skip_entries_count++] = skip;

			num_blocks++;
		}

		term_blocks[i].doc_freq	   = doc_count;
		term_blocks[i].block_count = (uint16)num_blocks;

		/* Free posting sources */
		{
			int j;

			for (j = 0; j < num_psources; j++)
				posting_source_free(&psources[j]);
			pfree(psources);
		}

		if ((i % 1000) == 0)
			CHECK_FOR_INTERRUPTS();
	}

	/* Write skip index */
	header.skip_index_offset = current_offset;
	if (skip_entries_count > 0)
	{
		BufFileWrite(
				file,
				all_skip_entries,
				skip_entries_count * sizeof(TpSkipEntry));
		current_offset += skip_entries_count * sizeof(TpSkipEntry);
	}
	pfree(all_skip_entries);

	/* Write fieldnorm table */
	header.fieldnorm_offset = current_offset;
	if (docmap->num_docs > 0)
	{
		BufFileWrite(
				file, docmap->fieldnorms, docmap->num_docs * sizeof(uint8));
		current_offset += docmap->num_docs * sizeof(uint8);
	}

	/* Write CTID pages array */
	header.ctid_pages_offset = current_offset;
	if (docmap->num_docs > 0)
	{
		BufFileWrite(
				file,
				docmap->ctid_pages,
				docmap->num_docs * sizeof(BlockNumber));
		current_offset += docmap->num_docs * sizeof(BlockNumber);
	}

	/* Write CTID offsets array */
	header.ctid_offsets_offset = current_offset;
	if (docmap->num_docs > 0)
	{
		BufFileWrite(
				file,
				docmap->ctid_offsets,
				docmap->num_docs * sizeof(OffsetNumber));
		current_offset += docmap->num_docs * sizeof(OffsetNumber);
	}

	/* Final data_size */
	header.data_size = current_offset;

	/* Seek back to write dict entries and header */
	{
		TpDictEntry *dict_entries;
		int			 end_fileno;
		off_t		 end_file_offset;

		BufFileTell(file, &end_fileno, &end_file_offset);

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

		{
			uint64 base_abs =
					tp_buffile_composite_offset(base_fileno, base_file_offset);
			int	  dict_fileno;
			off_t dict_offset;

			tp_buffile_decompose_offset(
					base_abs + header.entries_offset,
					&dict_fileno,
					&dict_offset);
			BufFileSeek(file, dict_fileno, dict_offset, SEEK_SET);
		}
		BufFileWrite(file, dict_entries, num_terms * sizeof(TpDictEntry));
		pfree(dict_entries);

		BufFileSeek(file, base_fileno, base_file_offset, SEEK_SET);
		BufFileWrite(file, &header, sizeof(TpSegmentHeader));

		BufFileSeek(file, end_fileno, end_file_offset, SEEK_SET);
	}

	/* Cleanup */
	pfree(string_offsets);
	pfree(term_blocks);
	free_merge_doc_mapping(&doc_mapping);
	tp_docmap_destroy(docmap);

	return current_offset;
}

/* ----------------------------------------------------------------
 * Worker-side BufFile compaction
 *
 * Mirrors tp_maybe_compact_level() but operates entirely within
 * BufFile. Opens BufFile readers for source segments, performs
 * N-way merge, writes result back to BufFile.
 * ----------------------------------------------------------------
 */
static void
worker_maybe_compact_level(
		WorkerSegmentTracker *tracker, BufFile *file, uint32 level)
{
	uint32 spl = (uint32)tp_segments_per_level;

	if (level >= TP_MAX_LEVELS - 1)
		return;

	while (tracker->level_counts[level] >= spl)
	{
		TpMergeSource	 *sources;
		TpSegmentReader **readers;
		int				  num_sources;
		TpMergedTerm	 *merged_terms	   = NULL;
		uint32			  num_merged_terms = 0;
		uint32			  merged_capacity  = 0;
		uint64			  total_tokens	   = 0;
		uint32			  collected		   = 0;
		uint32			  i;
		uint64			  new_data_size;
		MemoryContext	  merge_ctx;
		MemoryContext	  old_ctx;

		/* Collect spl non-consumed entries at this level */
		sources		= palloc0(sizeof(TpMergeSource) * spl);
		readers		= palloc0(sizeof(TpSegmentReader *) * spl);
		num_sources = 0;

		for (i = 0; i < tracker->count && collected < spl; i++)
		{
			WorkerSegmentEntry *e = &tracker->entries[i];

			if (e->consumed || e->level != level)
				continue;

			readers[collected] = tp_segment_open_from_buffile(file, e->offset);
			if (!readers[collected])
				continue;

			if (merge_source_init_from_reader(
						&sources[num_sources], readers[collected]))
			{
				total_tokens += readers[collected]->header->total_tokens;
				num_sources++;
			}
			collected++;
		}

		if (num_sources < 2)
		{
			/* Not enough valid sources to merge */
			for (i = 0; i < (uint32)num_sources; i++)
				merge_source_close(&sources[i]);
			for (i = 0; i < collected; i++)
			{
				if (readers[i])
					tp_segment_close(readers[i]);
			}
			pfree(sources);
			pfree(readers);
			break;
		}

		merge_ctx = AllocSetContextCreate(
				CurrentMemoryContext,
				"Worker BufFile Merge",
				ALLOCSET_DEFAULT_SIZES);
		old_ctx = MemoryContextSwitchTo(merge_ctx);

		/* N-way term merge */
		while (true)
		{
			int			  min_idx;
			const char	 *min_term;
			TpMergedTerm *current_merged;

			min_idx = merge_find_min_source(sources, num_sources);
			if (min_idx < 0)
				break;

			min_term = sources[min_idx].current_term;

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
							merged_terms,
							merged_capacity * sizeof(TpMergedTerm));
			}

			current_merged					 = &merged_terms[num_merged_terms];
			current_merged->term_len		 = strlen(min_term);
			current_merged->term			 = pstrdup(min_term);
			current_merged->segment_refs	 = NULL;
			current_merged->num_segment_refs = 0;
			current_merged->segment_refs_capacity = 0;
			current_merged->posting_offset		  = 0;
			current_merged->posting_count		  = 0;
			num_merged_terms++;

			for (i = 0; i < (uint32)num_sources; i++)
			{
				if (sources[i].exhausted)
					continue;

				if (strcmp(sources[i].current_term, current_merged->term) == 0)
				{
					merged_term_add_segment_ref(
							current_merged, i, &sources[i].current_entry);
					merge_source_advance(&sources[i]);
				}
			}

			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(old_ctx);

		/* Write merged segment to BufFile */
		if (num_merged_terms > 0)
		{
			int	   end_fileno;
			off_t  end_offset;
			uint64 new_offset;

			/* Seek to end of BufFile for appending */
			BufFileSeek(file, 0, 0, SEEK_END);
			BufFileTell(file, &end_fileno, &end_offset);
			new_offset = tp_buffile_composite_offset(end_fileno, end_offset);

			new_data_size = write_merged_segment_to_buffile(
					file,
					merged_terms,
					num_merged_terms,
					sources,
					num_sources,
					level + 1,
					total_tokens);

			/* Mark source segments consumed */
			collected = 0;
			for (i = 0; i < tracker->count && collected < spl; i++)
			{
				WorkerSegmentEntry *e = &tracker->entries[i];

				if (e->consumed || e->level != level)
					continue;

				e->consumed = true;
				collected++;
			}
			tracker->level_counts[level] -= spl;

			/* Add merged segment */
			tracker_add_segment(tracker, new_offset, new_data_size, level + 1);
		}

		/* Cleanup merge data */
		for (i = 0; i < num_merged_terms; i++)
		{
			if (merged_terms[i].term)
				pfree(merged_terms[i].term);
			if (merged_terms[i].segment_refs)
				pfree(merged_terms[i].segment_refs);
		}
		if (merged_terms)
			pfree(merged_terms);

		/* Close sources (don't close readers - merge_source_close
		 * doesn't own them for init_from_reader) */
		for (i = 0; i < (uint32)num_sources; i++)
		{
			if (sources[i].current_term)
				pfree(sources[i].current_term);
			if (sources[i].string_offsets)
				pfree(sources[i].string_offsets);
			sources[i].reader = NULL;
		}
		for (i = 0; i < collected; i++)
		{
			if (readers[i])
				tp_segment_close(readers[i]);
		}

		pfree(sources);
		pfree(readers);
		MemoryContextDelete(merge_ctx);
	}

	/* Recurse to check next level */
	worker_maybe_compact_level(tracker, file, level + 1);
}

/* ----------------------------------------------------------------
 * Shared memory estimation and initialization
 * ----------------------------------------------------------------
 */

Size
tp_parallel_build_estimate_shmem(
		Relation heap, Snapshot snapshot, int nworkers)
{
	Size size;

	/* Base shared structure */
	size = MAXALIGN(sizeof(TpParallelBuildShared));

	/* Per-worker result array */
	size = add_size(size, MAXALIGN(sizeof(TpParallelWorkerResult) * nworkers));

	/* Parallel table scan descriptor */
	size = add_size(size, table_parallelscan_estimate(heap, snapshot));

	return size;
}

static void
tp_init_parallel_shared(
		TpParallelBuildShared *shared,
		Relation			   heap,
		Relation			   index,
		Oid					   text_config_oid,
		AttrNumber			   attnum,
		double				   k1,
		double				   b,
		int					   nworkers)
{
	TpParallelWorkerResult *results;
	int						i;

	memset(shared, 0, sizeof(TpParallelBuildShared));

	/* Immutable configuration */
	shared->heaprelid		= RelationGetRelid(heap);
	shared->indexrelid		= RelationGetRelid(index);
	shared->text_config_oid = text_config_oid;
	shared->attnum			= attnum;
	shared->k1				= k1;
	shared->b				= b;
	shared->nworkers		= nworkers;

	/* Coordination */
	ConditionVariableInit(&shared->all_done_cv);
	pg_atomic_init_u32(&shared->workers_done, 0);
	pg_atomic_init_u64(&shared->tuples_done, 0);

	/* Two-phase coordination */
	pg_atomic_init_u32(&shared->phase1_done, 0);
	ConditionVariableInit(&shared->phase2_cv);
	pg_atomic_init_u32(&shared->phase2_ready, 0);
	pg_atomic_init_u64(&shared->next_page, 0);

	/* Initialize per-worker results */
	results = TpParallelWorkerResults(shared);
	for (i = 0; i < nworkers; i++)
		memset(&results[i], 0, sizeof(TpParallelWorkerResult));
}

/* ----------------------------------------------------------------
 * Write a temp file segment to contiguous index pages
 * ----------------------------------------------------------------
 */
static BlockNumber
write_temp_segment_to_index(
		Relation index, BufFile *file, uint64 base_offset, uint64 data_size)
{
	TpSegmentWriter writer;
	BlockNumber		header_block;
	BlockNumber		page_index_root;
	char			buf[TP_COPY_BUF_SIZE];
	uint64			remaining;
	int				seek_fileno;
	off_t			seek_offset;

	/* Initialize writer — extends relation via P_NEW */
	tp_segment_writer_init(&writer, index);
	header_block = writer.pages[0];

	/* Stream data from BufFile through writer */
	tp_buffile_decompose_offset(base_offset, &seek_fileno, &seek_offset);
	BufFileSeek(file, seek_fileno, seek_offset, SEEK_SET);
	remaining = data_size;
	while (remaining > 0)
	{
		uint32 chunk = (uint32)Min(remaining, TP_COPY_BUF_SIZE);

		BufFileReadExact(file, buf, chunk);
		tp_segment_writer_write(&writer, buf, chunk);
		remaining -= chunk;
	}

	/* Flush remaining buffered data */
	tp_segment_writer_flush(&writer);
	writer.buffer_pos = SizeOfPageHeaderData;

	/* Write page index */
	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);

	/* Update segment header on the first page */
	{
		Buffer			 header_buf;
		Page			 header_page;
		TpSegmentHeader *hdr;

		header_buf = ReadBuffer(index, header_block);
		LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
		header_page = BufferGetPage(header_buf);
		hdr			= (TpSegmentHeader *)PageGetContents(header_page);

		hdr->num_pages	= writer.pages_allocated;
		hdr->page_index = page_index_root;
		hdr->data_size	= writer.current_offset;

		MarkBufferDirty(header_buf);
		UnlockReleaseBuffer(header_buf);
	}

	tp_segment_writer_finish(&writer);

	/* Free writer pages array */
	if (writer.pages)
		pfree(writer.pages);

	return header_block;
}

/* ----------------------------------------------------------------
 * Write a temp file segment to index pages using atomic counter
 *
 * Same as write_temp_segment_to_index but uses pre-allocated pages
 * via atomic page counter instead of FSM/P_NEW.
 * ----------------------------------------------------------------
 */
static BlockNumber
write_temp_segment_to_index_parallel(
		Relation		  index,
		BufFile			 *file,
		uint64			  base_offset,
		uint64			  data_size,
		pg_atomic_uint64 *page_counter)
{
	TpSegmentWriter writer;
	BlockNumber		header_block;
	BlockNumber		page_index_root;
	char			buf[TP_COPY_BUF_SIZE];
	uint64			remaining;
	int				seek_fileno;
	off_t			seek_offset;

	/* Initialize writer with atomic page counter */
	tp_segment_writer_init_parallel(&writer, index, page_counter);
	header_block = writer.pages[0];

	/* Stream data from BufFile through writer */
	tp_buffile_decompose_offset(base_offset, &seek_fileno, &seek_offset);
	BufFileSeek(file, seek_fileno, seek_offset, SEEK_SET);
	remaining = data_size;
	while (remaining > 0)
	{
		uint32 chunk = (uint32)Min(remaining, TP_COPY_BUF_SIZE);

		BufFileReadExact(file, buf, chunk);
		tp_segment_writer_write(&writer, buf, chunk);
		remaining -= chunk;
	}

	/* Flush remaining buffered data */
	tp_segment_writer_flush(&writer);
	writer.buffer_pos = SizeOfPageHeaderData;

	/* Write page index using atomic counter */
	page_index_root = write_page_index_with_counter(
			index, writer.pages, writer.pages_allocated, page_counter);

	/* Update segment header on the first page */
	{
		Buffer			 header_buf;
		Page			 header_page;
		TpSegmentHeader *hdr;

		header_buf = ReadBuffer(index, header_block);
		LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
		header_page = BufferGetPage(header_buf);
		hdr			= (TpSegmentHeader *)PageGetContents(header_page);

		hdr->num_pages	= writer.pages_allocated;
		hdr->page_index = page_index_root;
		hdr->data_size	= writer.current_offset;

		MarkBufferDirty(header_buf);
		UnlockReleaseBuffer(header_buf);
	}

	tp_segment_writer_finish(&writer);

	/* Free writer pages array */
	if (writer.pages)
		pfree(writer.pages);

	return header_block;
}

/*
 * Compute total index pages needed for a worker's segments.
 * Counts data pages + page index pages for each segment.
 */
static uint64
compute_pages_needed(TpParallelWorkerResult *result)
{
	uint64 total = 0;
	uint32 epp	 = tp_page_index_entries_per_page();
	uint32 s;

	for (s = 0; s < result->final_segment_count; s++)
	{
		uint64 data_size  = result->seg_sizes[s];
		uint32 data_pages = (uint32)((data_size + SEGMENT_DATA_PER_PAGE - 1) /
									 SEGMENT_DATA_PER_PAGE);
		uint32 pi_pages	  = (data_pages + epp - 1) / epp;

		total += data_pages + pi_pages;
	}

	return total;
}

/* ----------------------------------------------------------------
 * Worker entry point
 * ----------------------------------------------------------------
 */
PGDLLEXPORT void
tp_parallel_build_worker_main(dsm_segment *seg, shm_toc *toc)
{
	TpParallelBuildShared  *shared;
	TpParallelWorkerResult *my_result;
	Relation				heap;
	Relation				index;
	TableScanDesc			scan;
	TupleTableSlot		   *slot;
	TpBuildContext		   *build_ctx;
	MemoryContext			build_tmpctx;
	MemoryContext			oldctx;
	Size					budget;
	int						worker_id;
	BufFile				   *buffile;
	char					file_name[64];
	WorkerSegmentTracker	tracker;

	/* Attach to shared memory */
	shared = (TpParallelBuildShared *)
			shm_toc_lookup(toc, TP_PARALLEL_KEY_SHARED, false);

	worker_id = ParallelWorkerNumber;
	my_result = &TpParallelWorkerResults(shared)[worker_id];

	/* Open heap and index relations */
	heap  = table_open(shared->heaprelid, AccessShareLock);
	index = index_open(shared->indexrelid, AccessExclusiveLock);

	/* Attach to SharedFileSet and create worker's BufFile */
	SharedFileSetAttach(&shared->fileset, seg);
	snprintf(file_name, sizeof(file_name), "tp_worker_%d", worker_id);
	buffile = BufFileCreateFileSet(&shared->fileset.fs, file_name);

	/* Join parallel table scan */
	scan = table_beginscan_parallel(heap, TpParallelTableScan(shared));
	slot = table_slot_create(heap, NULL);

	/*
	 * Per-worker memory budget: split maintenance_work_mem across
	 * workers. Minimum 64MB per worker to avoid excessive flushing.
	 */
	budget = (Size)maintenance_work_mem * 1024L / shared->nworkers;
	if (budget < 64L * 1024 * 1024)
		budget = 64L * 1024 * 1024;

	build_ctx = tp_build_context_create(budget);
	tracker_init(&tracker);

	build_tmpctx = AllocSetContextCreate(
			CurrentMemoryContext,
			"parallel build per-doc temp",
			ALLOCSET_DEFAULT_SIZES);

	/* Process tuples */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Datum		text_datum;
		text	   *document_text;
		ItemPointer ctid;
		Datum		tsvector_datum;
		TSVector	tsvector;
		char	  **terms;
		int32	   *frequencies;
		int			term_count;
		int			doc_length;

		text_datum = slot_getattr(slot, shared->attnum, &isnull);
		if (isnull)
			goto next_tuple;

		document_text = DatumGetTextPP(text_datum);
		slot_getallattrs(slot);
		ctid = &slot->tts_tid;

		if (!ItemPointerIsValid(ctid))
			goto next_tuple;

		/* Tokenize in temporary context */
		oldctx = MemoryContextSwitchTo(build_tmpctx);

		tsvector_datum = DirectFunctionCall2Coll(
				to_tsvector_byid,
				InvalidOid,
				ObjectIdGetDatum(shared->text_config_oid),
				PointerGetDatum(document_text));
		tsvector = DatumGetTSVector(tsvector_datum);

		doc_length = tp_extract_terms_from_tsvector(
				tsvector, &terms, &frequencies, &term_count);

		MemoryContextSwitchTo(oldctx);

		if (term_count > 0)
		{
			tp_build_context_add_document(
					build_ctx,
					terms,
					frequencies,
					term_count,
					doc_length,
					ctid);
		}

		/* Reset per-doc context */
		MemoryContextReset(build_tmpctx);

		/* Budget-based flush to BufFile */
		if (tp_build_context_should_flush(build_ctx))
		{
			uint64 data_size;
			int	   fileno;
			off_t  file_offset;

			my_result->total_docs += build_ctx->num_docs;
			my_result->total_len += build_ctx->total_len;

			/* Record offset before writing */
			BufFileTell(buffile, &fileno, &file_offset);

			data_size = tp_write_segment_to_buffile(build_ctx, buffile);
			my_result->segment_count++;

			/* Track this L0 segment */
			tracker_add_segment(
					&tracker,
					tp_buffile_composite_offset(fileno, file_offset),
					data_size,
					0);

			tp_build_context_reset(build_ctx);

			/* Run compaction if L0 is full */
			worker_maybe_compact_level(&tracker, buffile, 0);
		}

	next_tuple:
		my_result->tuples_scanned++;

		if (my_result->tuples_scanned % TP_PROGRESS_REPORT_INTERVAL == 0)
		{
			pg_atomic_add_fetch_u64(
					&shared->tuples_done, TP_PROGRESS_REPORT_INTERVAL);
			CHECK_FOR_INTERRUPTS();
		}
	}

	/* Flush remaining data */
	if (build_ctx->num_docs > 0)
	{
		uint64 data_size;
		int	   fileno;
		off_t  file_offset;

		my_result->total_docs += build_ctx->num_docs;
		my_result->total_len += build_ctx->total_len;

		BufFileTell(buffile, &fileno, &file_offset);

		data_size = tp_write_segment_to_buffile(build_ctx, buffile);
		my_result->segment_count++;

		tracker_add_segment(
				&tracker,
				tp_buffile_composite_offset(fileno, file_offset),
				data_size,
				0);

		/* Final compaction pass */
		worker_maybe_compact_level(&tracker, buffile, 0);
	}

	/*
	 * Phase 1 complete: report non-consumed segments to leader.
	 * These are the final segments after worker-side compaction.
	 */
	{
		uint32 seg_idx = 0;
		uint32 i;

		for (i = 0; i < tracker.count; i++)
		{
			WorkerSegmentEntry *e = &tracker.entries[i];

			if (e->consumed)
				continue;

			if (seg_idx >= TP_MAX_WORKER_SEGMENTS)
			{
				elog(WARNING,
					 "worker %d has too many segments (%u), "
					 "truncating",
					 worker_id,
					 tracker.count);
				break;
			}

			my_result->seg_offsets[seg_idx] = e->offset;
			my_result->seg_sizes[seg_idx]	= e->data_size;
			my_result->seg_levels[seg_idx]	= e->level;
			seg_idx++;
		}
		my_result->final_segment_count = seg_idx;
	}

	/* Export BufFile so leader can reopen if needed */
	BufFileExportFileSet(buffile);
	BufFileClose(buffile);

	/* Compute total pages this worker needs for Phase 2 */
	my_result->total_pages_needed = compute_pages_needed(my_result);

	/* Cleanup Phase 1 resources (keep heap/index open for Phase 2) */
	tp_build_context_destroy(build_ctx);
	tracker_destroy(&tracker);
	MemoryContextDelete(build_tmpctx);

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);

	/* Signal Phase 1 done, wait for leader to pre-extend */
	pg_atomic_fetch_add_u32(&shared->phase1_done, 1);
	ConditionVariableBroadcast(&shared->all_done_cv);

	ConditionVariablePrepareToSleep(&shared->phase2_cv);
	while (pg_atomic_read_u32(&shared->phase2_ready) == 0)
		ConditionVariableSleep(&shared->phase2_cv, PG_WAIT_EXTENSION);
	ConditionVariableCancelSleep();

	/*
	 * Phase 2: write segments to pre-allocated index pages.
	 * Reopen own BufFile read-only and write each segment
	 * using atomic page counter.
	 */
	if (my_result->final_segment_count > 0)
	{
		BufFile *rdfile;
		uint32	 s;

		snprintf(file_name, sizeof(file_name), "tp_worker_%d", worker_id);
		rdfile = BufFileOpenFileSet(
				&shared->fileset.fs, file_name, O_RDONLY, false);

		for (s = 0; s < my_result->final_segment_count; s++)
		{
			BlockNumber seg_root;

			seg_root = write_temp_segment_to_index_parallel(
					index,
					rdfile,
					my_result->seg_offsets[s],
					my_result->seg_sizes[s],
					&shared->next_page);

			/* Set level in segment header */
			{
				Buffer			 hdr_buf;
				Page			 hdr_page;
				TpSegmentHeader *hdr;

				hdr_buf = ReadBuffer(index, seg_root);
				LockBuffer(hdr_buf, BUFFER_LOCK_EXCLUSIVE);
				hdr_page   = BufferGetPage(hdr_buf);
				hdr		   = (TpSegmentHeader *)PageGetContents(hdr_page);
				hdr->level = my_result->seg_levels[s];
				MarkBufferDirty(hdr_buf);
				UnlockReleaseBuffer(hdr_buf);
			}

			my_result->seg_roots[s] = seg_root;
		}

		BufFileClose(rdfile);
	}

	index_close(index, AccessExclusiveLock);
	table_close(heap, AccessShareLock);

	/* Signal all work complete */
	pg_atomic_fetch_add_u32(&shared->workers_done, 1);
	ConditionVariableSignal(&shared->all_done_cv);
}

/* ----------------------------------------------------------------
 * Leader: main parallel build entry point
 * ----------------------------------------------------------------
 */
IndexBuildResult *
tp_build_parallel(
		Relation   heap,
		Relation   index,
		IndexInfo *indexInfo,
		Oid		   text_config_oid,
		double	   k1,
		double	   b,
		int		   nworkers)
{
	IndexBuildResult	  *result;
	ParallelContext		  *pcxt;
	TpParallelBuildShared *shared;
	Snapshot			   snapshot;
	Size				   shmem_size;
	int					   launched;
	uint64				   total_docs = 0;
	uint64				   total_len  = 0;

	/* Ensure reasonable number of workers */
	if (nworkers > TP_MAX_PARALLEL_WORKERS)
		nworkers = TP_MAX_PARALLEL_WORKERS;

	/* Report loading phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);

	/* Report estimated tuple count for progress tracking */
	{
		double reltuples  = heap->rd_rel->reltuples;
		int64  tuples_est = (reltuples > 0) ? (int64)reltuples : 0;

		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_TOTAL, tuples_est);
	}

	/* Get snapshot for parallel scan */
	snapshot = GetTransactionSnapshot();
#if PG_VERSION_NUM >= 180000
	/* PG18: Must register the snapshot for index builds */
	snapshot = RegisterSnapshot(snapshot);
#endif

	/* Calculate shared memory size */
	shmem_size = tp_parallel_build_estimate_shmem(heap, snapshot, nworkers);

	/* Enter parallel mode and create context */
	EnterParallelMode();
	pcxt = CreateParallelContext(
			"pg_textsearch", "tp_parallel_build_worker_main", nworkers);

	/* Estimate and allocate shared memory */
	shm_toc_estimate_chunk(&pcxt->estimator, shmem_size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	InitializeParallelDSM(pcxt);

	/* Allocate and initialize shared state */
	shared = (TpParallelBuildShared *)shm_toc_allocate(pcxt->toc, shmem_size);
	tp_init_parallel_shared(
			shared,
			heap,
			index,
			text_config_oid,
			indexInfo->ii_IndexAttrNumbers[0],
			k1,
			b,
			nworkers);

	/* Initialize SharedFileSet for worker temp files */
	SharedFileSetInit(&shared->fileset, pcxt->seg);

	/* Initialize parallel table scan */
	table_parallelscan_initialize(heap, TpParallelTableScan(shared), snapshot);

	/* Insert shared state into TOC */
	shm_toc_insert(pcxt->toc, TP_PARALLEL_KEY_SHARED, shared);

	/* Launch workers */
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;

	ereport(NOTICE,
			(errmsg("parallel index build: launched %d of %d "
					"requested workers",
					launched,
					nworkers)));

	/*
	 * Phase 1 wait: wait for all workers to finish BufFile phase.
	 * Workers signal phase1_done and then block on phase2_cv.
	 */
	ConditionVariablePrepareToSleep(&shared->all_done_cv);
	while (pg_atomic_read_u32(&shared->phase1_done) < (uint32)launched)
	{
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_DONE,
				(int64)pg_atomic_read_u64(&shared->tuples_done));

		ConditionVariableTimedSleep(
				&shared->all_done_cv, 1000 /* ms */, PG_WAIT_EXTENSION);
	}
	ConditionVariableCancelSleep();

	/* Collect corpus stats from all workers */
	{
		TpParallelWorkerResult *results;
		int						i;

		results = TpParallelWorkerResults(shared);
		for (i = 0; i < launched; i++)
		{
			total_docs += results[i].total_docs;
			total_len += results[i].total_len;
		}
	}

	/* Report final tuple count */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_TUPLES_DONE, (int64)total_docs);

	/* Report writing phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_WRITING);

	/*
	 * Pre-extend relation for all worker segments.
	 * Sum total_pages_needed across workers and extend in batches.
	 */
	{
		TpParallelWorkerResult *results		= TpParallelWorkerResults(shared);
		uint64					total_pages = 0;
		BlockNumber				start_blk;
		int						i;

#define EXTEND_BATCH_SIZE 8192

		for (i = 0; i < launched; i++)
			total_pages += results[i].total_pages_needed;

		start_blk = RelationGetNumberOfBlocks(index);

		/* Extend relation in batches to limit buffer pin count */
		{
			uint64 remaining = total_pages;

			while (remaining > 0)
			{
				uint32	batch = (uint32)Min(remaining, EXTEND_BATCH_SIZE);
				Buffer *bufs  = palloc(batch * sizeof(Buffer));
				uint32	extended;
				uint32	j;

				ExtendBufferedRelBy(
						BMR_REL(index),
						MAIN_FORKNUM,
						NULL,
						0,
						batch,
						bufs,
						&extended);

				for (j = 0; j < extended; j++)
				{
					LockBuffer(bufs[j], BUFFER_LOCK_EXCLUSIVE);
					PageInit(BufferGetPage(bufs[j]), BLCKSZ, 0);
					MarkBufferDirty(bufs[j]);
					UnlockReleaseBuffer(bufs[j]);
				}
				pfree(bufs);
				remaining -= extended;
			}
		}

		/* Set atomic page counter to the start of pre-allocated region */
		pg_atomic_write_u64(&shared->next_page, (uint64)start_blk);
	}

	/* Signal Phase 2: pages are ready */
	pg_atomic_write_u32(&shared->phase2_ready, 1);
	ConditionVariableBroadcast(&shared->phase2_cv);

	/*
	 * Phase 2 wait: workers write segments to index pages.
	 * Wait for all workers to complete.
	 */
	ConditionVariablePrepareToSleep(&shared->all_done_cv);
	while (pg_atomic_read_u32(&shared->workers_done) < (uint32)launched)
	{
		ConditionVariableTimedSleep(
				&shared->all_done_cv, 1000 /* ms */, PG_WAIT_EXTENSION);
	}
	ConditionVariableCancelSleep();
	WaitForParallelWorkersToFinish(pcxt);

	/* Report compaction phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_COMPACTING);

	/*
	 * Read seg_roots[] from each worker and chain segments into
	 * per-level linked lists. No BufFile I/O needed — workers
	 * already wrote segments to index pages.
	 */
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;
		BlockNumber		level_heads[TP_MAX_LEVELS];
		BlockNumber		level_tails[TP_MAX_LEVELS];
		uint16			level_counts[TP_MAX_LEVELS];
		int				i;

		TpParallelWorkerResult *results = TpParallelWorkerResults(shared);

		for (i = 0; i < TP_MAX_LEVELS; i++)
		{
			level_heads[i]	= InvalidBlockNumber;
			level_tails[i]	= InvalidBlockNumber;
			level_counts[i] = 0;
		}

		for (i = 0; i < launched; i++)
		{
			uint32 s;

			for (s = 0; s < results[i].final_segment_count; s++)
			{
				uint32		level	 = results[i].seg_levels[s];
				BlockNumber seg_root = results[i].seg_roots[s];

				/* Chain into per-level linked list */
				if (level_tails[level] != InvalidBlockNumber)
				{
					Buffer			 tail_buf;
					Page			 tail_page;
					TpSegmentHeader *tail_hdr;

					tail_buf = ReadBuffer(index, level_tails[level]);
					LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
					tail_page = BufferGetPage(tail_buf);
					tail_hdr  = (TpSegmentHeader *)PageGetContents(tail_page);
					tail_hdr->next_segment = seg_root;
					MarkBufferDirty(tail_buf);
					UnlockReleaseBuffer(tail_buf);
				}
				else
				{
					level_heads[level] = seg_root;
				}
				level_tails[level] = seg_root;
				level_counts[level]++;
			}
		}

		/* Update metapage with level chains and corpus stats */
		metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		for (i = 0; i < TP_MAX_LEVELS; i++)
		{
			metap->level_heads[i]  = level_heads[i];
			metap->level_counts[i] = level_counts[i];
		}
		metap->total_docs = total_docs;
		metap->total_len  = total_len;

		MarkBufferDirty(metabuf);
		UnlockReleaseBuffer(metabuf);

		/*
		 * Run final compaction on combined per-level counts.
		 * Workers compacted within their own segment sets, but
		 * combined counts may exceed segments_per_level.
		 * E.g., 4 workers each with 3 leftover L0s = 12 L0s.
		 */
		for (i = 0; i < TP_MAX_LEVELS - 1; i++)
		{
			if (level_counts[i] >= (uint16)tp_segments_per_level)
				tp_maybe_compact_level(index, (uint32)i);
		}
	}

	/* Flush all relation buffers to disk */
	FlushRelationBuffers(index);

	/* Build result */
	result				 = palloc0(sizeof(IndexBuildResult));
	result->heap_tuples	 = (double)total_docs;
	result->index_tuples = (double)total_docs;

	/* Cleanup */
	DestroyParallelContext(pcxt);
	ExitParallelMode();
#if PG_VERSION_NUM >= 180000
	UnregisterSnapshot(snapshot);
#endif

	return result;
}
