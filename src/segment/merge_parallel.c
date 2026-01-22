/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment/merge_parallel.c - Parallel segment compaction implementation
 *
 * This implements parallel compaction where each worker performs a full N-way
 * merge (default 8-way per tp_segments_per_level), producing segments that go
 * directly to the next level.
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/parallel.h>
#include <access/relation.h>
#include <access/xact.h>
#include <catalog/storage.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/condition_variable.h>
#include <storage/indexfsm.h>
#include <storage/smgr.h>
#include <utils/guc.h>
#include <utils/inval.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/timestamp.h>

#include "compression.h"
#include "constants.h"
#include "docmap.h"
#include "fieldnorm.h"
#include "merge.h"
#include "merge_parallel.h"
#include "pagemapper.h"
#include "segment.h"
#include "state/metapage.h"

/* External: segments per level GUC */
extern int tp_segments_per_level;

/* External: compression GUC from mod.c */
extern bool tp_compress_segments;

/* External: parallel build expansion factor GUC */
extern double tp_parallel_build_expansion_factor;

/* Forward declarations */
static void
tp_assign_compaction_tasks(TpParallelCompactShared *shared, Relation index);

static BlockNumber tp_worker_merge_segments(
		TpParallelCompactShared *shared, Relation index, TpCompactTask *task);

static void tp_preallocate_compact_pool(
		Relation index, TpParallelCompactShared *shared, int total_pages);

static void
tp_link_compacted_segments(TpParallelCompactShared *shared, Relation index);

static int estimate_compact_pool_pages(Relation index, uint32 total_segments);

/*
 * Check if parallel compaction should be used.
 *
 * Returns true if there are enough segments across levels to benefit from
 * parallel compaction. Sets total_segments to the count.
 */
bool
tp_should_compact_parallel(Relation index, uint32 *total_segments)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	uint32			total = 0;
	uint32			tasks = 0;
	int				level;

	/* Check if parallel maintenance is enabled */
	if (max_parallel_maintenance_workers <= 0)
		return false;

	/* Read metapage to count segments */
	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	/* Count segments and potential tasks */
	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		uint32 count = metap->level_counts[level];
		total += count;

		/* Each full merge is a potential parallel task */
		if (count >= (uint32)tp_segments_per_level)
			tasks += count / tp_segments_per_level;
	}

	UnlockReleaseBuffer(metabuf);

	*total_segments = total;

	/*
	 * Use parallel compaction only if we have at least 2 tasks worth of work.
	 * For smaller workloads, serial compaction is faster due to coordination
	 * overhead.
	 */
	return tasks >= 2;
}

/*
 * Estimate shared memory size for parallel compaction.
 */
static Size
tp_parallel_compact_estimate_shmem(int num_tasks, int pool_pages)
{
	Size size;

	/* Base shared structure */
	size = MAXALIGN(sizeof(TpParallelCompactShared));

	/* Results array */
	size = add_size(size, MAXALIGN(sizeof(TpCompactResult) * num_tasks));

	/* Page pool */
	size = add_size(size, MAXALIGN((Size)pool_pages * sizeof(BlockNumber)));

	return size;
}

/*
 * Main entry point for parallel compaction.
 *
 * Scans all levels, generates tasks for levels needing compaction, and
 * launches workers to perform parallel merges. After completion, links all
 * output segments into their target levels.
 */
void
tp_compact_parallel(Relation index)
{
	ParallelContext			*pcxt;
	TpParallelCompactShared *shared;
	Size					 shmem_size;
	int						 nworkers;
	int						 launched;
	int						 total_pool_pages;
	uint32					 total_segments;

	/* Verify we should use parallel compaction */
	if (!tp_should_compact_parallel(index, &total_segments))
	{
		/* Fall back to serial compaction */
		tp_maybe_compact_level(index, 0);
		return;
	}

	nworkers = Min(max_parallel_maintenance_workers, TP_MAX_COMPACT_WORKERS);

	/* Estimate pool pages needed */
	total_pool_pages = estimate_compact_pool_pages(index, total_segments);

	/* Estimate shared memory size */
	shmem_size = tp_parallel_compact_estimate_shmem(
			TP_MAX_COMPACT_WORKERS * TP_MAX_LEVELS, total_pool_pages);

	/* Enter parallel mode */
	EnterParallelMode();
	pcxt = CreateParallelContext(
			"pg_textsearch", "tp_parallel_compact_worker_main", nworkers);

	/* Estimate and allocate shared memory */
	shm_toc_estimate_chunk(&pcxt->estimator, shmem_size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	InitializeParallelDSM(pcxt);

	/* Allocate and initialize shared state */
	shared = (TpParallelCompactShared *)
			shm_toc_allocate(pcxt->toc, shmem_size);

	memset(shared,
		   0,
		   shmem_size); /* Zero ALL shared memory including results */
	shared->indexrelid		   = RelationGetRelid(index);
	shared->segments_per_merge = tp_segments_per_level;
	shared->total_pool_pages   = total_pool_pages;

	SpinLockInit(&shared->task_mutex);
	SpinLockInit(&shared->done_mutex);
	ConditionVariableInit(&shared->cv);
	pg_atomic_init_u32(&shared->next_task, 0);
	pg_atomic_init_u32(&shared->workers_done, 0);
	pg_atomic_init_u32(&shared->tasks_completed, 0);
	pg_atomic_init_u32(&shared->error_occurred, 0);
	pg_atomic_init_u32(&shared->pool_next, 0);

	/* Pre-allocate page pool */
	tp_preallocate_compact_pool(index, shared, total_pool_pages);

	/* Assign compaction tasks */
	tp_assign_compaction_tasks(shared, index);

	if (shared->num_tasks == 0)
	{
		/* No tasks to execute - clean up and return */
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}

	shared->worker_count = Min(nworkers + 1, (int)shared->num_tasks);

	/* Insert shared state into TOC */
	shm_toc_insert(pcxt->toc, TP_PARALLEL_COMPACT_KEY_SHARED, shared);

	/* Launch workers */
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;

	elog(DEBUG1,
		 "Parallel compaction: %u tasks, requested %d workers, launched %d",
		 shared->num_tasks,
		 nworkers,
		 launched);

	/*
	 * Leader participates as a worker too.
	 * This is important for the case where no background workers are launched.
	 */
	{
		Relation idx = index_open(shared->indexrelid, RowExclusiveLock);

		while (true)
		{
			uint32		   task_idx;
			TpCompactTask *task;
			BlockNumber	   result;

			/* Pull next task atomically */
			task_idx = pg_atomic_fetch_add_u32(&shared->next_task, 1);
			if (task_idx >= shared->num_tasks)
				break; /* No more tasks */

			task = &shared->tasks[task_idx];

			/* Perform the merge */
			result = tp_worker_merge_segments(shared, idx, task);

			/* Store result */
			if (result != InvalidBlockNumber)
			{
				TpCompactResult *res = &TpCompactResults(shared)[task_idx];
				res->output_segment	 = result;
				res->target_level	 = task->target_level;
				res->valid			 = true;
			}

			pg_atomic_fetch_add_u32(&shared->tasks_completed, 1);
			CHECK_FOR_INTERRUPTS();
		}

		index_close(idx, RowExclusiveLock);
	}

	/* Signal leader is done */
	pg_atomic_fetch_add_u32(&shared->workers_done, 1);

	/* Wait for all workers */
	WaitForParallelWorkersToFinish(pcxt);

	/* Check for errors */
	if (pg_atomic_read_u32(&shared->error_occurred))
	{
		elog(WARNING,
			 "Parallel compaction encountered errors: %s",
			 shared->error_message);
		/* Fall back to serial for remaining work */
	}

	/* Link all successfully compacted segments */
	tp_link_compacted_segments(shared, index);

	/* Truncate unused pool pages */
	{
		uint32		 pool_used	= pg_atomic_read_u32(&shared->pool_next);
		uint32		 pool_total = shared->total_pool_pages;
		BlockNumber *pool		= TpCompactPagePool(shared);

		if (pool_used < pool_total && pool_used > 0)
		{
			BlockNumber truncate_to = pool[0] + pool_used;
			BlockNumber old_nblocks = RelationGetNumberOfBlocks(index);
			ForkNumber	forknum		= MAIN_FORKNUM;

			if (truncate_to < old_nblocks)
			{
#if PG_VERSION_NUM >= 180000
				smgrtruncate(
						RelationGetSmgr(index),
						&forknum,
						1,
						&old_nblocks,
						&truncate_to);
#else
				smgrtruncate(
						RelationGetSmgr(index), &forknum, 1, &truncate_to);
#endif
				CacheInvalidateRelcache(index);
			}
		}
	}

	/* Cleanup */
	DestroyParallelContext(pcxt);
	ExitParallelMode();

	/* Check if more compaction is needed at higher levels */
	tp_maybe_compact_level(index, 0);
}

/*
 * Estimate pages needed for compaction pool.
 *
 * Each merged segment needs pages for dictionary, postings, skip index,
 * fieldnorm, and CTID maps. We estimate based on source segment sizes.
 */
static int
estimate_compact_pool_pages(
		Relation index, uint32 total_segments __attribute__((unused)))
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	uint32			estimated_pages = 0;
	int				level;

	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	/* Estimate based on existing segments */
	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber current = metap->level_heads[level];
		while (current != InvalidBlockNumber)
		{
			Buffer			 seg_buf;
			Page			 seg_page;
			TpSegmentHeader *header;

			seg_buf = ReadBuffer(index, current);
			LockBuffer(seg_buf, BUFFER_LOCK_SHARE);
			seg_page = BufferGetPage(seg_buf);
			header	 = (TpSegmentHeader *)((char *)seg_page +
										   SizeOfPageHeaderData);

			estimated_pages += header->num_pages;
			current = header->next_segment;

			UnlockReleaseBuffer(seg_buf);
		}
	}

	UnlockReleaseBuffer(metabuf);

	/*
	 * Add expansion factor for page index pages and some headroom.
	 * Merged segments may be slightly larger due to alignment overhead.
	 */
	estimated_pages = (uint32)(estimated_pages *
							   (1.0 + tp_parallel_build_expansion_factor));

	/* Minimum allocation */
	if (estimated_pages < 64)
		estimated_pages = 64;

	return (int)estimated_pages;
}

/*
 * Pre-allocate page pool for parallel compaction.
 */
static void
tp_preallocate_compact_pool(
		Relation index, TpParallelCompactShared *shared, int total_pages)
{
	int			 i;
	Buffer		 buf;
	BlockNumber *pool;

	pool = TpCompactPagePool(shared);

	for (i = 0; i < total_pages; i++)
	{
		buf = ReadBufferExtended(
				index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
		pool[i] = BufferGetBlockNumber(buf);
		PageInit(BufferGetPage(buf), BLCKSZ, 0);
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
	}

	smgrimmedsync(RelationGetSmgr(index), MAIN_FORKNUM);
}

/*
 * Assign compaction tasks based on current segment distribution.
 *
 * Each task performs a full N-way merge (N = segments_per_level).
 * If leftover segments < 50% of N, they're absorbed into the last merge.
 */
static void
tp_assign_compaction_tasks(TpParallelCompactShared *shared, Relation index)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	uint32			merge_size = shared->segments_per_merge;
	int				level;

	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	shared->num_tasks = 0;

	for (level = 0; level < TP_MAX_LEVELS - 1; level++)
	{
		uint32		count = metap->level_counts[level];
		uint32		full_merges;
		uint32		runt;
		uint32		i;
		BlockNumber current;
		BlockNumber segments
				[TP_MAX_SEGMENTS_PER_TASK * (TP_MAX_COMPACT_WORKERS + 1)];
		uint32 seg_idx = 0;
		bool   absorb_runt;

		if (count < merge_size)
			continue; /* Level doesn't need compaction */

		/* Collect all segments at this level */
		current = metap->level_heads[level];
		while (current != InvalidBlockNumber && current != 0 &&
			   seg_idx < count)
		{
			Buffer			 seg_buf;
			Page			 seg_page;
			TpSegmentHeader *header;

			segments[seg_idx++] = current;

			seg_buf = ReadBuffer(index, current);
			LockBuffer(seg_buf, BUFFER_LOCK_SHARE);
			seg_page = BufferGetPage(seg_buf);
			header	 = (TpSegmentHeader *)((char *)seg_page +
										   SizeOfPageHeaderData);

			/* Validate segment magic to catch corruption early */
			if (header->magic != TP_SEGMENT_MAGIC)
			{
				UnlockReleaseBuffer(seg_buf);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid segment at block %u during task "
								"assignment",
								current),
						 errdetail(
								 "magic=0x%08X, expected 0x%08X",
								 header->magic,
								 TP_SEGMENT_MAGIC)));
			}

			current = header->next_segment;
			UnlockReleaseBuffer(seg_buf);
		}

		/* Calculate number of full merges and runt */
		full_merges = count / merge_size;
		runt		= count % merge_size;

		/*
		 * Runt handling:
		 * - If runt < 50% of merge_size, absorb into last full merge
		 * - If runt >= 50%, create a separate smaller task for it
		 */
		absorb_runt = (runt > 0 && runt < merge_size / 2 && full_merges > 0);

		/* Create tasks for full merges */
		seg_idx = 0;
		for (i = 0; i < full_merges; i++)
		{
			TpCompactTask *task;
			uint32		   task_segs = merge_size;
			uint32		   j;

			/* Last full task absorbs runt if applicable */
			if (i == full_merges - 1 && absorb_runt)
				task_segs = merge_size + runt;

			if (shared->num_tasks >= TP_MAX_COMPACT_WORKERS * TP_MAX_LEVELS)
				break;

			task			   = &shared->tasks[shared->num_tasks++];
			task->source_level = level;
			task->target_level = level + 1;
			task->num_segments = task_segs;
			task->assigned	   = false;

			for (j = 0; j < task_segs && seg_idx < count; j++)
				task->source_segments[j] = segments[seg_idx++];
		}

		/* Create task for remaining runt if not absorbed */
		if (runt > 0 && !absorb_runt)
		{
			TpCompactTask *task;
			uint32		   j;

			if (shared->num_tasks < TP_MAX_COMPACT_WORKERS * TP_MAX_LEVELS)
			{
				task			   = &shared->tasks[shared->num_tasks++];
				task->source_level = level;
				task->target_level = level + 1;
				task->num_segments = runt;
				task->assigned	   = false;

				for (j = 0; j < runt && seg_idx < count; j++)
					task->source_segments[j] = segments[seg_idx++];
			}
		}
	}

	UnlockReleaseBuffer(metabuf);

	elog(DEBUG1, "Parallel compaction: assigned %u tasks", shared->num_tasks);
}

/*
 * Worker entry point - called by parallel infrastructure.
 */
PGDLLEXPORT void
tp_parallel_compact_worker_main(dsm_segment *seg, shm_toc *toc)
{
	TpParallelCompactShared *shared;
	Relation				 index;

	(void)seg; /* unused */

	shared = (TpParallelCompactShared *)
			shm_toc_lookup(toc, TP_PARALLEL_COMPACT_KEY_SHARED, false);

	index = index_open(shared->indexrelid, RowExclusiveLock);

	/* Refresh smgr cache to see pre-allocated pages */
	smgrnblocks(RelationGetSmgr(index), MAIN_FORKNUM);

	while (true)
	{
		uint32		   task_idx;
		TpCompactTask *task;
		BlockNumber	   result;

		/* Pull next task atomically */
		task_idx = pg_atomic_fetch_add_u32(&shared->next_task, 1);
		if (task_idx >= shared->num_tasks)
			break; /* No more tasks */

		task = &shared->tasks[task_idx];

		/* Perform the merge */
		PG_TRY();
		{
			result = tp_worker_merge_segments(shared, index, task);

			/* Store result */
			if (result != InvalidBlockNumber)
			{
				TpCompactResult *res = &TpCompactResults(shared)[task_idx];
				res->output_segment	 = result;
				res->target_level	 = task->target_level;
				res->valid			 = true;
			}
		}
		PG_CATCH();
		{
			/* Record error for leader to handle */
			pg_atomic_write_u32(&shared->error_occurred, 1);
			snprintf(
					shared->error_message,
					sizeof(shared->error_message),
					"Worker error during task %u",
					task_idx);
			PG_RE_THROW();
		}
		PG_END_TRY();

		pg_atomic_fetch_add_u32(&shared->tasks_completed, 1);
		CHECK_FOR_INTERRUPTS();
	}

	/* Signal completion */
	pg_atomic_fetch_add_u32(&shared->workers_done, 1);
	ConditionVariableSignal(&shared->cv);

	index_close(index, RowExclusiveLock);
}

/*
 * Perform an N-way merge for a single compaction task.
 *
 * This is the core merge logic extracted from tp_merge_level_segments,
 * adapted to work with the parallel compaction infrastructure.
 */
static BlockNumber
tp_worker_merge_segments(
		TpParallelCompactShared *shared, Relation index, TpCompactTask *task)
{
	/*
	 * This implementation delegates to tp_merge_segments_to_pool which
	 * does the actual N-way merge using the shared page pool.
	 */
	return tp_merge_segments_to_pool(
			index,
			task->source_segments,
			task->num_segments,
			task->target_level,
			TpCompactPagePool(shared),
			shared->total_pool_pages,
			&shared->pool_next);
}

/*
 * Link all compacted segments into their target levels.
 *
 * This is called by the leader after all workers finish. It updates the
 * metapage to remove source segments and add merged segments, then frees
 * pages from source segments.
 *
 * IMPORTANT: Pages must be freed AFTER updating the metapage to avoid
 * corruption if we crash in between.
 */
static void
tp_link_compacted_segments(TpParallelCompactShared *shared, Relation index)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	uint32			i;

	/* Collect pages to free - do this before modifying metapage */
	BlockNumber **pages_to_free;
	uint32		 *page_counts;
	uint32		  num_segments_to_free = 0;
	uint32		  total_pages_to_free  = 0;

	/* First pass: collect all pages from source segments */
	for (i = 0; i < shared->num_tasks; i++)
	{
		TpCompactResult *result = &TpCompactResults(shared)[i];
		TpCompactTask	*task	= &shared->tasks[i];

		if (!result->valid)
			continue;
		num_segments_to_free += task->num_segments;
	}

	pages_to_free = palloc0(sizeof(BlockNumber *) * num_segments_to_free);
	page_counts	  = palloc0(sizeof(uint32) * num_segments_to_free);

	{
		uint32 seg_idx = 0;
		for (i = 0; i < shared->num_tasks; i++)
		{
			TpCompactResult *result = &TpCompactResults(shared)[i];
			TpCompactTask	*task	= &shared->tasks[i];
			uint32			 j;

			if (!result->valid)
				continue;

			for (j = 0; j < task->num_segments; j++)
			{
				BlockNumber seg_root = task->source_segments[j];
				page_counts[seg_idx] = tp_segment_collect_pages(
						index, seg_root, &pages_to_free[seg_idx]);
				total_pages_to_free += page_counts[seg_idx];
				seg_idx++;
			}
		}
	}

	/* Second pass: update metapage */
	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	for (i = 0; i < shared->num_tasks; i++)
	{
		TpCompactResult *result = &TpCompactResults(shared)[i];
		TpCompactTask	*task	= &shared->tasks[i];

		if (!result->valid)
			continue;

		/* Clear source level counts (segments are now merged) */
		if (task->source_level < TP_MAX_LEVELS)
		{
			if (metap->level_counts[task->source_level] >= task->num_segments)
				metap->level_counts[task->source_level] -= task->num_segments;
			else
				metap->level_counts[task->source_level] = 0;

			/* If level is now empty, clear the head */
			if (metap->level_counts[task->source_level] == 0)
				metap->level_heads[task->source_level] = InvalidBlockNumber;
		}

		/* Add merged segment to target level */
		if (result->target_level < TP_MAX_LEVELS)
		{
			if (metap->level_heads[result->target_level] != InvalidBlockNumber)
			{
				/* Link to existing chain */
				Buffer			 seg_buf;
				Page			 seg_page;
				TpSegmentHeader *seg_header;

				seg_buf = ReadBuffer(index, result->output_segment);
				LockBuffer(seg_buf, BUFFER_LOCK_EXCLUSIVE);
				seg_page   = BufferGetPage(seg_buf);
				seg_header = (TpSegmentHeader *)((char *)seg_page +
												 SizeOfPageHeaderData);
				seg_header->next_segment =
						metap->level_heads[result->target_level];
				MarkBufferDirty(seg_buf);
				UnlockReleaseBuffer(seg_buf);
			}

			metap->level_heads[result->target_level] = result->output_segment;
			metap->level_counts[result->target_level]++;
		}
	}

	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	/* Third pass: free pages from source segments (AFTER metapage update) */
	for (i = 0; i < num_segments_to_free; i++)
	{
		if (pages_to_free[i] && page_counts[i] > 0)
		{
			tp_segment_free_pages(index, pages_to_free[i], page_counts[i]);
			pfree(pages_to_free[i]);
		}
	}
	pfree(pages_to_free);
	pfree(page_counts);

	/* Update FSM if we freed pages */
	if (total_pages_to_free > 0)
		IndexFreeSpaceMapVacuum(index);

	elog(DEBUG1,
		 "Parallel compaction linked %u tasks, freed %u pages",
		 shared->num_tasks,
		 total_pages_to_free);
}
