/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.c - Two-phase parallel index build with
 * cross-worker compaction
 *
 * Architecture:
 * - Phase 1: Each worker scans a heap partition, builds a local
 *   TpBuildContext, flushes segments to BufFile, and compacts
 *   within BufFile. Workers report segment info.
 * - Barrier: Leader plans cross-worker merge groups, sums pages,
 *   pre-extends relation with ExtendBufferedRelBy, sets atomic
 *   next_page counter, signals Phase 2.
 * - Phase 2: Workers COPY their non-merged segments to pages,
 *   then work-steal merge groups. Each task bulk-claims a
 *   contiguous page range from the shared counter.
 * - Leader truncates unused pool pages, links segments into
 *   per-level chains, updates metapage.
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <catalog/storage.h>
#include <commands/progress.h>
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
#include "segment/segment_io.h"
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
	uint64				buffile_end; /* logical end of BufFile */
} WorkerSegmentTracker;

static void
tracker_init(WorkerSegmentTracker *tracker)
{
	tracker->capacity	 = 32;
	tracker->count		 = 0;
	tracker->buffile_end = 0;
	tracker->entries = palloc(tracker->capacity * sizeof(WorkerSegmentEntry));
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
 * Worker-side BufFile compaction
 *
 * Mirrors tp_maybe_compact_level() but operates entirely within
 * BufFile. Opens BufFile readers for source segments, performs
 * N-way merge, writes result back to BufFile using TpMergeSink.
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

		/* Write merged segment to BufFile via sink */
		if (num_merged_terms > 0)
		{
			TpMergeSink sink;
			int			end_fileno;
			off_t		end_offset;
			uint64		new_offset;
			uint64		new_data_size;

			/*
			 * Position at logical end of BufFile for appending.
			 * Do NOT use SEEK_END: BufFileSeek(SEEK_END) calls
			 * FileSize() before flushing the dirty buffer, so it
			 * returns the on-disk size which may be less than the
			 * logical size. Use our tracked buffile_end instead.
			 */
			new_offset = tracker->buffile_end;
			tp_buffile_decompose_offset(new_offset, &end_fileno, &end_offset);
			BufFileSeek(file, end_fileno, end_offset, SEEK_SET);

			merge_sink_init_buffile(&sink, file);
			write_merged_segment_to_sink(
					&sink,
					merged_terms,
					num_merged_terms,
					sources,
					num_sources,
					level + 1,
					total_tokens,
					false);
			new_data_size = sink.current_offset;

			/* Update logical end of BufFile */
			tracker->buffile_end = new_offset + new_data_size;

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

	/* Base shared structure (includes per-worker block ranges) */
	size = MAXALIGN(sizeof(TpParallelBuildShared));

	/* Per-worker result array */
	size = add_size(size, MAXALIGN(sizeof(TpParallelWorkerResult) * nworkers));

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

	/* TID range scan coordination */
	shared->nworkers_launched = 0;
	pg_atomic_init_u32(&shared->scan_ready, 0);

	/* Two-phase coordination */
	pg_atomic_init_u32(&shared->phase1_done, 0);
	ConditionVariableInit(&shared->phase2_cv);
	pg_atomic_init_u32(&shared->phase2_ready, 0);
	pg_atomic_init_u64(&shared->next_page, 0);

	/* Cross-worker merge groups */
	shared->num_merge_groups = 0;
	pg_atomic_init_u32(&shared->next_merge_group, 0);
	{
		int mg;

		for (mg = 0; mg < TP_MAX_MERGE_GROUPS; mg++)
			shared->merge_group_results[mg] = InvalidBlockNumber;
	}

	/* Initialize per-worker results */
	results = TpParallelWorkerResults(shared);
	for (i = 0; i < nworkers; i++)
		memset(&results[i], 0, sizeof(TpParallelWorkerResult));
}

/* ----------------------------------------------------------------
 * Write a temp file segment to index pages using atomic counter
 *
 * Uses pre-allocated pages via atomic page counter instead of
 * FSM/P_NEW, ensuring contiguous allocation from the pool.
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
 * Compute index pages needed for one segment of given data_size.
 * Counts root/header page + data pages + page index pages.
 */
static uint64
compute_segment_pages(uint64 data_size)
{
	uint32 epp		  = tp_page_index_entries_per_page();
	uint32 data_pages = (uint32)((data_size + SEGMENT_DATA_PER_PAGE - 1) /
								 SEGMENT_DATA_PER_PAGE);
	/* Page index indexes root + data pages */
	uint32 pi_pages = (1 + data_pages + epp - 1) / epp;

	return 1 + (uint64)data_pages + pi_pages;
}

/*
 * Plan cross-worker merge groups after Phase 1.
 *
 * Collects all worker segments grouped by level, then forms merge
 * groups of segments_per_level segments at each level (bottom-up).
 * Remaining segments stay tagged 0 (COPY). Merge group outputs
 * go to level+1, which may cascade into further groups.
 *
 * Each segment's seg_merge_group[s] is set to:
 *   0    = COPY to pages at original level
 *   N>0  = member of merge group N (1-indexed)
 */
static void
plan_merge_groups(
		TpParallelBuildShared  *shared,
		TpParallelWorkerResult *results,
		int						nworkers)
{
	uint32 spl		 = (uint32)tp_segments_per_level;
	uint32 max_total = (uint32)nworkers * TP_MAX_WORKER_SEGMENTS;
	uint32 level_counts[TP_MAX_LEVELS];
	uint32 num_groups = 0;
	int	   level;

	/* (worker_id, seg_idx) pairs per level */
	typedef struct
	{
		uint16 worker_id;
		uint16 seg_idx;
	} SegRef;

	SegRef *level_segs[TP_MAX_LEVELS];

	memset(level_counts, 0, sizeof(level_counts));

	/* Allocate per-level arrays */
	for (level = 0; level < TP_MAX_LEVELS; level++)
		level_segs[level] = palloc(max_total * sizeof(SegRef));

	/* Initialize all merge_group tags to 0 (COPY) */
	{
		int w;

		for (w = 0; w < nworkers; w++)
		{
			memset(results[w].seg_merge_group,
				   0,
				   sizeof(results[w].seg_merge_group));
		}
	}

	/* Collect all segments grouped by level */
	{
		int w;

		for (w = 0; w < nworkers; w++)
		{
			uint32 s;

			for (s = 0; s < results[w].final_segment_count; s++)
			{
				uint32 lv  = results[w].seg_levels[s];
				uint32 idx = level_counts[lv];

				level_segs[lv][idx].worker_id = (uint16)w;
				level_segs[lv][idx].seg_idx	  = (uint16)s;
				level_counts[lv]++;
			}
		}
	}

	/* Bottom-up: form merge groups at each level */
	for (level = 0; level < TP_MAX_LEVELS - 1; level++)
	{
		uint32 count = level_counts[level];
		uint32 pos	 = 0;

		while (count - pos >= spl)
		{
			uint32 g;
			uint32 k;

			if (num_groups >= TP_MAX_MERGE_GROUPS)
			{
				elog(WARNING,
					 "parallel build: too many merge groups "
					 "(%u), stopping at level %d",
					 num_groups,
					 level);
				goto done;
			}

			g							  = num_groups++;
			shared->merge_group_levels[g] = level + 1;

			/* Tag spl segments as members of this group */
			for (k = 0; k < spl; k++)
			{
				SegRef *ref = &level_segs[level][pos + k];
				int		w	= ref->worker_id;
				int		s	= ref->seg_idx;

				results[w].seg_merge_group[s] = g + 1;
			}

			/*
			 * The merge output goes to level+1. Add a
			 * placeholder entry so cascading can pick it up.
			 * We use worker_id=0xFFFF as sentinel (no real
			 * segment — the merge result is stored in
			 * merge_group_results[g]).
			 */
			{
				uint32 next_lv = level + 1;
				uint32 idx	   = level_counts[next_lv];

				level_segs[next_lv][idx].worker_id = 0xFFFF;
				level_segs[next_lv][idx].seg_idx   = (uint16)g;
				level_counts[next_lv]++;
			}

			pos += spl;
		}

		/*
		 * Merge leftover segments at this level (fewer than spl).
		 * This reduces L0 fragmentation so fewer segments remain
		 * after build. Unlike full groups, remainder groups do NOT
		 * cascade to higher levels because worker_execute_merge_group
		 * only searches worker result arrays for sources, not the
		 * outputs of earlier merge groups.
		 */
		if (count - pos >= 2)
		{
			uint32 remainder = count - pos;
			uint32 g;
			uint32 k;

			if (num_groups >= TP_MAX_MERGE_GROUPS)
				goto done;

			g							  = num_groups++;
			shared->merge_group_levels[g] = level + 1;

			for (k = 0; k < remainder; k++)
			{
				SegRef *ref = &level_segs[level][pos + k];
				int		w	= ref->worker_id;
				int		s	= ref->seg_idx;

				results[w].seg_merge_group[s] = g + 1;
			}

			pos += remainder;
		}
	}

done:
	shared->num_merge_groups = num_groups;
	pg_atomic_write_u32(&shared->next_merge_group, 0);

	/* Free per-level arrays */
	for (level = 0; level < TP_MAX_LEVELS; level++)
		pfree(level_segs[level]);
}

/*
 * Compute total pages needed for Phase 2.
 * Sums exact pages for COPY segments and upper-bound pages
 * for merge groups (sum of source sizes).
 */
static uint64
compute_total_pages_needed(
		TpParallelBuildShared  *shared,
		TpParallelWorkerResult *results,
		int						nworkers)
{
	uint64 total = 0;
	int	   w;
	uint32 g;

	/* COPY segments: exact page counts */
	for (w = 0; w < nworkers; w++)
	{
		uint32 s;

		for (s = 0; s < results[w].final_segment_count; s++)
		{
			if (results[w].seg_merge_group[s] == 0)
				total += compute_segment_pages(results[w].seg_sizes[s]);
		}
	}

	/* Merge groups: upper-bound (sum of source sizes) + margin */
	for (g = 0; g < shared->num_merge_groups; g++)
	{
		uint64 sum_sizes = 0;
		uint64 group_pages;

		for (w = 0; w < nworkers; w++)
		{
			uint32 s;

			for (s = 0; s < results[w].final_segment_count; s++)
			{
				if (results[w].seg_merge_group[s] == g + 1)
					sum_sizes += results[w].seg_sizes[s];
			}
		}
		group_pages = compute_segment_pages(sum_sizes);

		/*
		 * Add 5% safety margin for merge groups.
		 * Cross-worker merge remaps doc IDs into a wider range,
		 * increasing bitpacked delta widths at worker boundaries.
		 * This makes the merged output up to ~1-2% larger than
		 * the sum of source sizes. Unused pages are truncated.
		 */
		group_pages += group_pages / 20;
		total += group_pages;
	}

	return total;
}

/*
 * Execute one cross-worker merge group.
 *
 * Called by workers in Phase 2 via work-stealing. Opens source
 * segments from multiple workers' BufFiles, performs N-way merge,
 * writes output directly to pre-allocated index pages.
 */
static void
worker_execute_merge_group(
		TpParallelBuildShared *shared, Relation index, uint32 group_idx)
{
	TpParallelWorkerResult *results	 = TpParallelWorkerResults(shared);
	int						nworkers = shared->nworkers_launched;

	/* Collect source segments for this group */
	typedef struct
	{
		int	   worker_id;
		uint32 seg_idx;
	} MergeRef;

	MergeRef *refs;
	uint32	  num_refs	   = 0;
	uint64	  sum_sizes	   = 0;
	uint64	  total_tokens = 0;
	uint32	  target_level;
	int		  w;
	uint32	  i;

	TpSegmentReader **readers;
	TpMergeSource	 *sources;
	int				  num_sources;
	BufFile			**open_files;
	bool			 *file_opened;

	TpMergedTerm *merged_terms	   = NULL;
	uint32		  num_merged_terms = 0;
	uint32		  merged_capacity  = 0;
	MemoryContext merge_ctx;
	MemoryContext old_ctx;

	uint64			 pages_needed;
	uint64			 pool_start;
	pg_atomic_uint64 local_counter;
	target_level = shared->merge_group_levels[group_idx];

	/* Count refs and sum sizes */
	refs = palloc(
			sizeof(MergeRef) * (uint32)nworkers * TP_MAX_WORKER_SEGMENTS);
	for (w = 0; w < nworkers; w++)
	{
		uint32 s;

		for (s = 0; s < results[w].final_segment_count; s++)
		{
			if (results[w].seg_merge_group[s] == group_idx + 1)
			{
				refs[num_refs].worker_id = w;
				refs[num_refs].seg_idx	 = s;
				num_refs++;
				sum_sizes += results[w].seg_sizes[s];
			}
		}
	}

	if (num_refs == 0)
	{
		pfree(refs);
		shared->merge_group_results[group_idx] = InvalidBlockNumber;
		return;
	}

	/* Bulk-claim contiguous page range (with 5% margin) */
	pages_needed = compute_segment_pages(sum_sizes);
	pages_needed += pages_needed / 20;
	pool_start = pg_atomic_fetch_add_u64(&shared->next_page, pages_needed);
	pg_atomic_init_u64(&local_counter, pool_start);

	/* Open needed worker BufFiles (one per worker, cached) */
	open_files	= palloc0(sizeof(BufFile *) * nworkers);
	file_opened = palloc0(sizeof(bool) * nworkers);

	readers		= palloc0(sizeof(TpSegmentReader *) * num_refs);
	sources		= palloc0(sizeof(TpMergeSource) * num_refs);
	num_sources = 0;

	for (i = 0; i < num_refs; i++)
	{
		int	   wid = refs[i].worker_id;
		uint32 s   = refs[i].seg_idx;

		if (!file_opened[wid])
		{
			char fname[64];

			snprintf(fname, sizeof(fname), "tp_worker_%d", wid);
			open_files[wid] = BufFileOpenFileSet(
					&shared->fileset.fs, fname, O_RDONLY, false);
			file_opened[wid] = true;
		}

		readers[i] = tp_segment_open_from_buffile(
				open_files[wid], results[wid].seg_offsets[s]);
		if (!readers[i])
			continue;

		if (merge_source_init_from_reader(&sources[num_sources], readers[i]))
		{
			total_tokens += readers[i]->header->total_tokens;
			num_sources++;
		}
	}

	if (num_sources == 0)
	{
		/* No valid sources — clean up and mark invalid */
		for (i = 0; i < num_refs; i++)
		{
			if (readers[i])
				tp_segment_close(readers[i]);
		}
		for (w = 0; w < nworkers; w++)
		{
			if (file_opened[w])
				BufFileClose(open_files[w]);
		}
		pfree(refs);
		pfree(readers);
		pfree(sources);
		pfree(open_files);
		pfree(file_opened);
		shared->merge_group_results[group_idx] = InvalidBlockNumber;
		return;
	}

	/* N-way term merge */
	merge_ctx = AllocSetContextCreate(
			CurrentMemoryContext,
			"Cross-worker Merge",
			ALLOCSET_DEFAULT_SIZES);
	old_ctx = MemoryContextSwitchTo(merge_ctx);

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
						merged_terms, merged_capacity * sizeof(TpMergedTerm));
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

	/* Write merged segment to pages */
	if (num_merged_terms > 0)
	{
		TpMergeSink sink;

		merge_sink_init_pages_parallel(&sink, index, &local_counter);

		write_merged_segment_to_sink(
				&sink,
				merged_terms,
				num_merged_terms,
				sources,
				num_sources,
				target_level,
				total_tokens,
				true);

		shared->merge_group_results[group_idx] = sink.writer.pages[0];

		if (sink.writer.pages)
			pfree(sink.writer.pages);
	}
	else
	{
		shared->merge_group_results[group_idx] = InvalidBlockNumber;
	}

	/* Cleanup */
	for (i = 0; i < num_merged_terms; i++)
	{
		if (merged_terms[i].term)
			pfree(merged_terms[i].term);
		if (merged_terms[i].segment_refs)
			pfree(merged_terms[i].segment_refs);
	}
	if (merged_terms)
		pfree(merged_terms);

	/* Close sources (don't close readers, init_from_reader) */
	for (i = 0; i < (uint32)num_sources; i++)
	{
		if (sources[i].current_term)
			pfree(sources[i].current_term);
		if (sources[i].string_offsets)
			pfree(sources[i].string_offsets);
		sources[i].reader = NULL;
	}
	for (i = 0; i < num_refs; i++)
	{
		if (readers[i])
			tp_segment_close(readers[i]);
	}
	for (w = 0; w < nworkers; w++)
	{
		if (file_opened[w])
			BufFileClose(open_files[w]);
	}

	MemoryContextDelete(merge_ctx);
	pfree(refs);
	pfree(readers);
	pfree(sources);
	pfree(open_files);
	pfree(file_opened);
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
	Snapshot				snap;
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

	/*
	 * Wait for leader to set up per-worker block ranges.
	 * This is a brief spin (microseconds) after launch.
	 */
	while (pg_atomic_read_u32(&shared->scan_ready) == 0)
		pg_usleep(100);
	pg_read_barrier();

	/*
	 * Open a TID range scan limited to this worker's block range.
	 * Each worker scans a contiguous, non-overlapping range of
	 * heap blocks, ensuring disjoint CTIDs across workers.
	 */
	snap = GetTransactionSnapshot();
#if PG_VERSION_NUM >= 180000
	snap = RegisterSnapshot(snap);
#endif

	slot = table_slot_create(heap, NULL);
	{
		BlockNumber start_blk = shared->worker_start_block[worker_id];
		BlockNumber end_blk	  = shared->worker_end_block[worker_id];

		if (start_blk < end_blk)
		{
			ItemPointerData min_tid, max_tid;

			ItemPointerSet(&min_tid, start_blk, FirstOffsetNumber);
			ItemPointerSet(&max_tid, end_blk - 1, MaxOffsetNumber);
			scan = table_beginscan_tidrange(heap, snap, &min_tid, &max_tid);
		}
		else
		{
			/* Empty range: no blocks assigned to this worker */
			scan = NULL;
		}
	}

	/*
	 * Per-worker memory budget: split maintenance_work_mem across
	 * workers. Minimum 64MB per worker to avoid excessive flushing.
	 */
	budget = (Size)maintenance_work_mem * 1024L / shared->nworkers_launched;
	if (budget < 64L * 1024 * 1024)
		budget = 64L * 1024 * 1024;

	build_ctx = tp_build_context_create(budget);
	tracker_init(&tracker);

	build_tmpctx = AllocSetContextCreate(
			CurrentMemoryContext,
			"parallel build per-doc temp",
			ALLOCSET_DEFAULT_SIZES);

	/* Process tuples from this worker's block range */
	while (scan != NULL &&
		   table_scan_getnextslot_tidrange(scan, ForwardScanDirection, slot))
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
			uint64 seg_offset;

			my_result->total_docs += build_ctx->num_docs;
			my_result->total_len += build_ctx->total_len;

			/* L0 segment starts at current end of BufFile */
			seg_offset = tracker.buffile_end;
			{
				int	  fileno;
				off_t file_offset;

				tp_buffile_decompose_offset(seg_offset, &fileno, &file_offset);
				BufFileSeek(buffile, fileno, file_offset, SEEK_SET);
			}

			data_size = tp_write_segment_to_buffile(build_ctx, buffile);
			my_result->segment_count++;

			/* Track this L0 segment */
			tracker_add_segment(&tracker, seg_offset, data_size, 0);
			tracker.buffile_end = seg_offset + data_size;

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
		uint64 seg_offset;

		my_result->total_docs += build_ctx->num_docs;
		my_result->total_len += build_ctx->total_len;

		seg_offset = tracker.buffile_end;
		{
			int	  fileno;
			off_t file_offset;

			tp_buffile_decompose_offset(seg_offset, &fileno, &file_offset);
			BufFileSeek(buffile, fileno, file_offset, SEEK_SET);
		}

		data_size = tp_write_segment_to_buffile(build_ctx, buffile);
		my_result->segment_count++;

		tracker_add_segment(&tracker, seg_offset, data_size, 0);
		tracker.buffile_end = seg_offset + data_size;

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

	/* Cleanup Phase 1 resources (keep heap/index open for Phase 2) */
	tp_build_context_destroy(build_ctx);
	tracker_destroy(&tracker);
	MemoryContextDelete(build_tmpctx);

	if (scan != NULL)
		table_endscan(scan);
#if PG_VERSION_NUM >= 180000
	UnregisterSnapshot(snap);
#endif
	ExecDropSingleTupleTableSlot(slot);

	/* Signal Phase 1 done, wait for leader to pre-extend */
	pg_atomic_fetch_add_u32(&shared->phase1_done, 1);
	ConditionVariableBroadcast(&shared->all_done_cv);

	ConditionVariablePrepareToSleep(&shared->phase2_cv);
	while (pg_atomic_read_u32(&shared->phase2_ready) == 0)
		ConditionVariableSleep(&shared->phase2_cv, PG_WAIT_EXTENSION);
	ConditionVariableCancelSleep();

	/*
	 * Phase 2: COPY own segments + work-steal merge groups.
	 *
	 * Step 1: Copy own non-merged segments to index pages.
	 * Each COPY bulk-claims a contiguous page range for the segment.
	 */
	{
		bool	 has_copy_segs = false;
		BufFile *rdfile		   = NULL;
		uint32	 s;

		for (s = 0; s < my_result->final_segment_count; s++)
		{
			if (my_result->seg_merge_group[s] == 0)
			{
				has_copy_segs = true;
				break;
			}
		}

		if (has_copy_segs)
		{
			snprintf(file_name, sizeof(file_name), "tp_worker_%d", worker_id);
			rdfile = BufFileOpenFileSet(
					&shared->fileset.fs, file_name, O_RDONLY, false);

			for (s = 0; s < my_result->final_segment_count; s++)
			{
				uint64			 pages;
				uint64			 pool_start;
				pg_atomic_uint64 local_counter;
				BlockNumber		 seg_root;

				if (my_result->seg_merge_group[s] != 0)
					continue;

				/* Bulk-claim contiguous pages for this segment */
				pages = compute_segment_pages(my_result->seg_sizes[s]);
				pool_start =
						pg_atomic_fetch_add_u64(&shared->next_page, pages);
				pg_atomic_init_u64(&local_counter, pool_start);

				seg_root = write_temp_segment_to_index_parallel(
						index,
						rdfile,
						my_result->seg_offsets[s],
						my_result->seg_sizes[s],
						&local_counter);

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
	}

	/*
	 * Step 2: Work-steal merge groups.
	 * Each worker claims groups via atomic counter until all done.
	 */
	{
		uint32 g;

		while ((g = pg_atomic_fetch_add_u32(&shared->next_merge_group, 1)) <
			   shared->num_merge_groups)
			worker_execute_merge_group(shared, index, g);
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

	/* Insert shared state into TOC */
	shm_toc_insert(pcxt->toc, TP_PARALLEL_KEY_SHARED, shared);

	/* Launch workers */
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;

	if (launched == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("parallel index build: could not launch "
						"any workers"),
				 errhint("Increase max_worker_processes or "
						 "reduce "
						 "max_parallel_maintenance_workers.")));

	ereport(NOTICE,
			(errmsg("parallel index build: launched %d of %d "
					"requested workers",
					launched,
					nworkers)));

	/*
	 * Compute per-worker block ranges for disjoint TID scans.
	 * Must be done after launch so we know `launched`. Workers
	 * spin on scan_ready until ranges are set.
	 */
	{
		BlockNumber nblocks			  = RelationGetNumberOfBlocks(heap);
		BlockNumber blocks_per_worker = nblocks / launched;
		BlockNumber remainder		  = nblocks % launched;
		BlockNumber cursor			  = 0;
		int			i;

		for (i = 0; i < launched; i++)
		{
			BlockNumber count = blocks_per_worker +
								(i < (int)remainder ? 1 : 0);
			shared->worker_start_block[i] = cursor;
			shared->worker_end_block[i]	  = cursor + count;
			cursor += count;
		}
		shared->nworkers_launched = launched;
		pg_write_barrier();
		pg_atomic_write_u32(&shared->scan_ready, 1);
	}

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
	 * Plan cross-worker merge groups and compute total pages.
	 */
	{
		TpParallelWorkerResult *results = TpParallelWorkerResults(shared);

		plan_merge_groups(shared, results, launched);
	}

	/*
	 * Pre-extend relation for all worker segments + merge groups.
	 */
	{
		TpParallelWorkerResult *results		= TpParallelWorkerResults(shared);
		uint64					total_pages = 0;
		BlockNumber				start_blk;

#define EXTEND_BATCH_SIZE 8192

		total_pages = compute_total_pages_needed(shared, results, launched);
		start_blk	= RelationGetNumberOfBlocks(index);

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

	/*
	 * Link segments into per-level chains in metapage.
	 * COPY segments go to their original level; merge group
	 * results go to merge_group_levels[g].
	 */
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;
		BlockNumber		level_heads[TP_MAX_LEVELS];
		BlockNumber		level_tails[TP_MAX_LEVELS];
		uint16			level_counts[TP_MAX_LEVELS];
		int				lv;
		int				i;
		uint32			g;

		TpParallelWorkerResult *results = TpParallelWorkerResults(shared);

		for (lv = 0; lv < TP_MAX_LEVELS; lv++)
		{
			level_heads[lv]	 = InvalidBlockNumber;
			level_tails[lv]	 = InvalidBlockNumber;
			level_counts[lv] = 0;
		}

		/* Helper macro: append seg_root to level chain */
#define APPEND_TO_LEVEL(seg_root, lv)                                  \
	do                                                                 \
	{                                                                  \
		if (level_tails[lv] != InvalidBlockNumber)                     \
		{                                                              \
			Buffer			 tb;                                       \
			Page			 tp;                                       \
			TpSegmentHeader *th;                                       \
                                                                       \
			tb = ReadBuffer(index, level_tails[lv]);                   \
			LockBuffer(tb, BUFFER_LOCK_EXCLUSIVE);                     \
			tp				 = BufferGetPage(tb);                      \
			th				 = (TpSegmentHeader *)PageGetContents(tp); \
			th->next_segment = (seg_root);                             \
			MarkBufferDirty(tb);                                       \
			UnlockReleaseBuffer(tb);                                   \
		}                                                              \
		else                                                           \
		{                                                              \
			level_heads[lv] = (seg_root);                              \
		}                                                              \
		level_tails[lv] = (seg_root);                                  \
		level_counts[lv]++;                                            \
	} while (0)

		/* Add COPY segments */
		for (i = 0; i < launched; i++)
		{
			uint32 s;

			for (s = 0; s < results[i].final_segment_count; s++)
			{
				if (results[i].seg_merge_group[s] != 0)
					continue;

				APPEND_TO_LEVEL(
						results[i].seg_roots[s], results[i].seg_levels[s]);
			}
		}

		/* Add merge group results */
		for (g = 0; g < shared->num_merge_groups; g++)
		{
			BlockNumber root = shared->merge_group_results[g];

			if (root == InvalidBlockNumber)
				continue;

			APPEND_TO_LEVEL(root, shared->merge_group_levels[g]);
		}

#undef APPEND_TO_LEVEL

		/* Update metapage with per-level chains and corpus stats */
		metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		for (lv = 0; lv < TP_MAX_LEVELS; lv++)
		{
			metap->level_heads[lv]	= level_heads[lv];
			metap->level_counts[lv] = level_counts[lv];
		}
		metap->total_docs = total_docs;
		metap->total_len  = total_len;

		MarkBufferDirty(metabuf);
		UnlockReleaseBuffer(metabuf);
	}

	tp_truncate_dead_pages(index);

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
