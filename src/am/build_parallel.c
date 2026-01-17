/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am/build_parallel.c - Parallel index build implementation
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
#include <storage/bufmgr.h>
#include <storage/condition_variable.h>
#include <storage/indexfsm.h>
#include <storage/smgr.h>
#include <tsearch/ts_type.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <utils/inval.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "am.h"
#include "build_parallel.h"
#include "constants.h"
#include "memtable/local_memtable.h"
#include "segment/docmap.h"
#include "segment/merge.h"
#include "segment/pagemapper.h"
#include "segment/segment.h"
#include "state/metapage.h"

/*
 * Callback wrapper for building docmap from local memtable
 */
static void
docmap_add_callback(ItemPointer ctid, int32 doc_length, void *arg)
{
	TpDocMapBuilder *docmap = (TpDocMapBuilder *)arg;
	tp_docmap_add(docmap, ctid, (uint32)doc_length);
}

/* Minimum pages to pre-allocate per worker */
#define TP_MIN_PAGES_PER_WORKER 64

/*
 * Expansion factor for estimating index pages from heap.
 *
 * BM25 indexes typically use 30-40% of heap pages. We use 0.8 to provide
 * adequate safety margin for large datasets. The pool also includes estimated
 * page index pages.
 *
 * If the pool is exhausted during build, an error will be raised suggesting
 * to increase this factor. The unused pool pages are reclaimed via truncation
 * after the build completes.
 */
#define TP_INDEX_EXPANSION_FACTOR 1.0

/*
 * Worker build state with double-buffering support.
 * Two memtables allow one to be filled while the other is being spilled.
 */
typedef struct TpWorkerBuildState
{
	TpLocalMemtable *memtable_a; /* First memtable */
	TpLocalMemtable *memtable_b; /* Second memtable */
	TpLocalMemtable *active;	 /* Currently being filled */
	TpLocalMemtable *spilling;	 /* Being written to segment (NULL if idle) */
} TpWorkerBuildState;

/*
 * Initialize double-buffered worker state
 */
static void
tp_worker_state_init(TpWorkerBuildState *state)
{
	state->memtable_a = tp_local_memtable_create();
	state->memtable_b = tp_local_memtable_create();
	state->active	  = state->memtable_a;
	state->spilling	  = NULL;
}

/*
 * Destroy worker state
 */
static void
tp_worker_state_destroy(TpWorkerBuildState *state)
{
	if (state->memtable_a)
		tp_local_memtable_destroy(state->memtable_a);
	if (state->memtable_b)
		tp_local_memtable_destroy(state->memtable_b);
	state->memtable_a = NULL;
	state->memtable_b = NULL;
	state->active	  = NULL;
	state->spilling	  = NULL;
}

/*
 * Get the alternate memtable (the one not currently active)
 */
static TpLocalMemtable *
tp_worker_state_get_alternate(TpWorkerBuildState *state)
{
	if (state->active == state->memtable_a)
		return state->memtable_b;
	else
		return state->memtable_a;
}

/*
 * Swap active memtable to the alternate one.
 * Returns the previously active memtable (for spilling).
 */
static TpLocalMemtable *
tp_worker_state_swap(TpWorkerBuildState *state)
{
	TpLocalMemtable *prev = state->active;
	state->active		  = tp_worker_state_get_alternate(state);
	state->spilling		  = prev;
	return prev;
}

/*
 * Forward declarations for local functions
 */
static void tp_init_parallel_shared(
		TpParallelBuildShared *shared,
		Relation			   heap,
		Relation			   index,
		Oid					   text_config_oid,
		AttrNumber			   attnum,
		double				   k1,
		double				   b,
		int					   nworkers);

static void tp_preallocate_page_pool(
		Relation index, TpParallelBuildShared *shared, int total_pages);

static void tp_leader_participate(
		TpParallelBuildShared *shared,
		Relation			   heap,
		Relation			   index,
		Snapshot			   snapshot);

static void
tp_link_all_worker_segments(TpParallelBuildShared *shared, Relation index);

static BlockNumber tp_write_segment_from_local_memtable(
		TpLocalMemtable		  *memtable,
		Relation			   index,
		TpParallelBuildShared *shared,
		int					   worker_id);

/*
 * Spill a memtable to disk and chain the resulting segment.
 * Returns true if a segment was written.
 */
static bool
tp_worker_spill_memtable(
		TpLocalMemtable		  *memtable,
		Relation			   index,
		TpParallelBuildShared *shared,
		int					   worker_id,
		TpWorkerSegmentInfo	  *my_info)
{
	BlockNumber seg_block;

	if (memtable->num_docs == 0)
		return false;

	elog(DEBUG1,
		 "Worker %d spilling memtable: %d docs, %ld postings",
		 worker_id,
		 memtable->num_docs,
		 memtable->total_postings);

	seg_block = tp_write_segment_from_local_memtable(
			memtable, index, shared, worker_id);

	if (seg_block == InvalidBlockNumber)
		return false;

	/* Chain segment into worker's list */
	if (my_info->segment_head == InvalidBlockNumber)
	{
		my_info->segment_head = seg_block;
		my_info->segment_tail = seg_block;
	}
	else
	{
		/* Link previous tail to new segment */
		Buffer			 tail_buf;
		Page			 tail_page;
		TpSegmentHeader *tail_header;

		tail_buf = ReadBuffer(index, my_info->segment_tail);
		LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
		tail_page				  = BufferGetPage(tail_buf);
		tail_header				  = (TpSegmentHeader *)((char *)tail_page +
											SizeOfPageHeaderData);
		tail_header->next_segment = seg_block;
		MarkBufferDirty(tail_buf);
		UnlockReleaseBuffer(tail_buf);

		my_info->segment_tail = seg_block;
	}
	my_info->segment_count++;

	/* Aggregate stats */
	my_info->docs_indexed += memtable->num_docs;
	my_info->total_len += memtable->total_len;

	return true;
}

static void
tp_finalize_parallel_stats(TpParallelBuildShared *shared, Relation index);

static BlockNumber tp_write_segment_from_local_memtable(
		TpLocalMemtable		  *memtable,
		Relation			   index,
		TpParallelBuildShared *shared,
		int					   worker_id);

static void tp_worker_process_document(
		TpLocalMemtable *memtable,
		TupleTableSlot	*slot,
		int				 attnum,
		Oid				 text_config_oid);

/*
 * Estimate shared memory needed for parallel build
 */
Size
tp_parallel_build_estimate_shmem(
		Relation heap, Snapshot snapshot, int nworkers, int total_pool_pages)
{
	Size size;
	int	 total_workers;

	/*
	 * Total workers = nworkers (background workers) + 1 (leader participates)
	 */
	total_workers = nworkers + 1;

	/* Base shared structure */
	size = MAXALIGN(sizeof(TpParallelBuildShared));

	/* Per-worker segment info array */
	size = add_size(
			size, MAXALIGN(sizeof(TpWorkerSegmentInfo) * total_workers));

	/* Shared page pool for all workers */
	size = add_size(
			size, MAXALIGN((Size)total_pool_pages * sizeof(BlockNumber)));

	/* Parallel table scan descriptor */
	size = add_size(size, table_parallelscan_estimate(heap, snapshot));

	return size;
}

/*
 * Main entry point for parallel index build
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
	int					   total_pool_pages;
	BlockNumber			   heap_pages;
	int					   launched;

	/* Ensure reasonable number of workers */
	if (nworkers > TP_MAX_PARALLEL_WORKERS)
		nworkers = TP_MAX_PARALLEL_WORKERS;

	/*
	 * Estimate total pages needed for index.
	 * Use heap size * expansion factor + minimum per worker.
	 * All workers share a single pool for better space efficiency.
	 *
	 * Also include estimated page index pages. Each segment needs
	 * ceil(segment_pages / entries_per_page) page index pages.
	 * With multiple workers creating multiple segments (due to spills),
	 * we estimate conservatively: assume each worker creates ~10 segments.
	 */
	heap_pages = RelationGetNumberOfBlocks(heap);
	{
		int data_pages;
		int entries_per_page;
		int page_index_pages;
		int estimated_segments;

		data_pages = (int)(heap_pages * TP_INDEX_EXPANSION_FACTOR) +
					 TP_MIN_PAGES_PER_WORKER * (nworkers + 1);
		entries_per_page   = tp_page_index_entries_per_page();
		estimated_segments = (nworkers + 1) * 10; /* ~10 segments per worker */

		/*
		 * Each segment needs at least 1 page index page.
		 * Plus pages for the actual data page mapping.
		 */
		page_index_pages = estimated_segments +
						   (data_pages + entries_per_page - 1) /
								   entries_per_page;

		total_pool_pages = data_pages + page_index_pages;
	}

	/* Get snapshot for parallel scan */
	snapshot = GetTransactionSnapshot();
	snapshot = RegisterSnapshot(snapshot);

	/* Calculate shared memory size */
	shmem_size = tp_parallel_build_estimate_shmem(
			heap, snapshot, nworkers, total_pool_pages);

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
	shared->total_pool_pages = total_pool_pages;

	/* Pre-allocate shared page pool */
	tp_preallocate_page_pool(index, shared, total_pool_pages);

	/* Initialize parallel table scan */
	table_parallelscan_initialize(heap, TpParallelTableScan(shared), snapshot);

	/* Insert shared state into TOC */
	shm_toc_insert(pcxt->toc, TP_PARALLEL_KEY_SHARED, shared);

	/* Launch workers */
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;

	elog(DEBUG1,
		 "Parallel index build: requested %d workers, launched %d",
		 nworkers,
		 launched);

	/* Leader participates as a worker too */
	if (launched > 0)
	{
		shared->leader_working = true;
		tp_leader_participate(shared, heap, index, snapshot);
	}
	else
	{
		/* No workers launched - fall back would be handled by caller */
		elog(WARNING, "No parallel workers launched for index build");
	}

	/* Wait for all workers to finish */
	WaitForParallelWorkersToFinish(pcxt);

	/*
	 * Reclaim unused pages from the pre-allocated pool.
	 * Pages beyond shared_pool_next were never used by any worker.
	 *
	 * Now that all allocations (including page index) come from the pool,
	 * unused pages are at the end of the relation. We can safely truncate
	 * to reclaim the space immediately.
	 *
	 * Pool pages are allocated contiguously via P_NEW, so:
	 *   pool[0] = first_pool_block (after metapage)
	 *   pool[i] = first_pool_block + i
	 *   unused pages = pool[pool_used] to pool[pool_total-1]
	 *
	 * The truncation point is pool[pool_used-1] + 1, which equals
	 * first_pool_block + pool_used = pool[0] + pool_used.
	 */
	{
		uint32		 pool_used = pg_atomic_read_u32(&shared->shared_pool_next);
		uint32		 pool_total = shared->total_pool_pages;
		BlockNumber *pool		= TpParallelPagePool(shared);

		if (pool_used < pool_total && pool_used > 0)
		{
			BlockNumber truncate_to;
			BlockNumber old_nblocks;
			uint32		unused	= pool_total - pool_used;
			ForkNumber	forknum = MAIN_FORKNUM;

			/*
			 * Truncate to just after the last used page.
			 * pool[0] is first pool block; pool[pool_used-1] is last used.
			 * Keep pool[0] + pool_used blocks total.
			 */
			truncate_to = pool[0] + pool_used;
			old_nblocks = RelationGetNumberOfBlocks(index);

			elog(DEBUG1,
				 "Truncating index: used %u of %u pool pages, "
				 "truncating from %u to %u blocks (reclaiming %u pages)",
				 pool_used,
				 pool_total,
				 old_nblocks,
				 truncate_to,
				 unused);

			/* Truncate the relation to reclaim unused space */
#if PG_VERSION_NUM >= 180000
			smgrtruncate(
					RelationGetSmgr(index),
					&forknum,
					1,
					&old_nblocks,
					&truncate_to);
#else
			smgrtruncate(RelationGetSmgr(index), &forknum, 1, &truncate_to);
#endif

			/* Invalidate relation cache to pick up new size */
			CacheInvalidateRelcache(index);

			elog(DEBUG1, "Truncated index, reclaimed %u pages", unused);
		}
		else if (pool_used == 0)
		{
			elog(WARNING, "Parallel build used 0 pool pages - no data?");
		}
	}

	/* Link all worker segments into L0 chain */
	tp_link_all_worker_segments(shared, index);

	/* Finalize statistics in metapage */
	tp_finalize_parallel_stats(shared, index);

	/* Build result */
	result				 = palloc0(sizeof(IndexBuildResult));
	result->heap_tuples	 = (double)pg_atomic_read_u64(&shared->tuples_scanned);
	result->index_tuples = (double)pg_atomic_read_u64(&shared->total_docs);

	/* Cleanup */
	DestroyParallelContext(pcxt);
	ExitParallelMode();
	UnregisterSnapshot(snapshot);

	return result;
}

/*
 * Initialize shared state for parallel build
 */
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
	TpWorkerSegmentInfo *worker_info;
	int					 i;

	memset(shared, 0, sizeof(TpParallelBuildShared));

	/* Immutable configuration */
	shared->heaprelid		= RelationGetRelid(heap);
	shared->indexrelid		= RelationGetRelid(index);
	shared->text_config_oid = text_config_oid;
	shared->attnum			= attnum;
	shared->k1				= k1;
	shared->b				= b;
	shared->worker_count	= nworkers + 1; /* nworkers + leader */

	elog(DEBUG1,
		 "Parallel build shared initialized: attnum=%d, workers=%d",
		 attnum,
		 shared->worker_count);

	/*
	 * Per-worker memory budget calculation.
	 *
	 * We split maintenance_work_mem across workers, with a factor of 2 for
	 * double-buffering (each worker can have up to 2 active memtables).
	 * We use 90% of the budget as the actual threshold to provide slop
	 * and avoid thrashing near the boundary.
	 */
#define TP_MEMORY_SLOP_FACTOR 0.9

	{
		Size memory_budget;
		int	 total_workers = nworkers + 1; /* background workers + leader */

		/* maintenance_work_mem is in KB, convert to bytes */
		memory_budget = ((Size)maintenance_work_mem * 1024) / total_workers /
						2;

		/* Apply slop factor to avoid thrashing near boundary */
		shared->memory_budget_per_worker = (Size)(memory_budget *
												  TP_MEMORY_SLOP_FACTOR);

		elog(DEBUG1,
			 "Parallel build: %d workers, %zu KB memory budget/worker",
			 total_workers,
			 shared->memory_budget_per_worker / 1024);
	}

	/* Initialize coordination primitives */
	SpinLockInit(&shared->mutex);
	ConditionVariableInit(&shared->workersdonecv);
	shared->workers_done   = 0;
	shared->leader_working = false;

	/* Initialize atomic counters */
	pg_atomic_init_u64(&shared->tuples_scanned, 0);
	pg_atomic_init_u64(&shared->total_docs, 0);
	pg_atomic_init_u64(&shared->total_len, 0);
	pg_atomic_init_u32(&shared->pool_exhausted, 0);
	pg_atomic_init_u32(&shared->shared_pool_next, 0);
	pg_atomic_init_u32(&shared->max_block_used, 0);

	/*
	 * Initialize worker segment info for all workers (bg workers + leader).
	 * Leader is worker_id=0, background workers are 1..nworkers.
	 */
	worker_info = TpParallelWorkerInfo(shared);
	for (i = 0; i < shared->worker_count; i++)
	{
		worker_info[i].segment_head	 = InvalidBlockNumber;
		worker_info[i].segment_tail	 = InvalidBlockNumber;
		worker_info[i].segment_count = 0;
		worker_info[i].docs_indexed	 = 0;
		worker_info[i].total_len	 = 0;
	}
}

/*
 * Pre-allocate shared page pool for all workers
 */
static void
tp_preallocate_page_pool(
		Relation index, TpParallelBuildShared *shared, int total_pages)
{
	int			 i;
	Buffer		 buf;
	BlockNumber *pool;

	pool = TpParallelPagePool(shared);

	elog(DEBUG1,
		 "Pre-allocating %d pages for parallel build shared pool",
		 total_pages);

	/* Extend relation and collect block numbers */
	for (i = 0; i < total_pages; i++)
	{
		buf = ReadBufferExtended(
				index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
		pool[i] = BufferGetBlockNumber(buf);

		/* Initialize page */
		PageInit(BufferGetPage(buf), BLCKSZ, 0);
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
	}

	/* Flush to ensure durability */
	smgrimmedsync(RelationGetSmgr(index), MAIN_FORKNUM);
}

/*
 * Worker entry point - called by parallel infrastructure
 *
 * Uses double-buffering: two memtables allow the worker to continue
 * processing documents while a previous memtable is being spilled.
 * In the current synchronous implementation, this primarily provides
 * cleaner code structure and prepares for future async I/O support.
 */
PGDLLEXPORT void
tp_parallel_build_worker_main(dsm_segment *seg, shm_toc *toc)
{
	TpParallelBuildShared *shared;
	TpWorkerSegmentInfo	  *my_info;
	Relation			   heap;
	Relation			   index;
	TableScanDesc		   scan;
	TupleTableSlot		  *slot;
	TpWorkerBuildState	   build_state;
	int					   worker_id;
	int64				   tuples_processed = 0;

	/* Attach to shared memory */
	shared = (TpParallelBuildShared *)
			shm_toc_lookup(toc, TP_PARALLEL_KEY_SHARED, false);

	/*
	 * Worker ID assignment: if leader participates, it claims worker_id=0.
	 * Background workers get IDs starting from 1 to avoid collision.
	 */
	worker_id = shared->leader_working ? ParallelWorkerNumber + 1
									   : ParallelWorkerNumber;
	my_info	  = &TpParallelWorkerInfo(shared)[worker_id];

	elog(DEBUG1, "Parallel build worker %d starting", worker_id);

	/* Open relations */
	heap  = table_open(shared->heaprelid, AccessShareLock);
	index = index_open(shared->indexrelid, RowExclusiveLock);

	/*
	 * Force smgr to refresh its cached nblocks. The leader pre-allocated pool
	 * pages which extended the relation, but this worker's smgr cache is
	 * stale. Without this, ReadBuffer can fail with "unexpected data beyond
	 * EOF".
	 */
	smgrnblocks(RelationGetSmgr(index), MAIN_FORKNUM);

	/* Enable parallel build mode - disables FSM for page allocation */
	tp_set_parallel_build_mode(true);

	/* Initialize double-buffered worker state */
	tp_worker_state_init(&build_state);

	/* Join parallel table scan */
	scan = table_beginscan_parallel(heap, TpParallelTableScan(shared));
	slot = table_slot_create(heap, NULL);

	/* Process tuples */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		/*
		 * Check memory budget BEFORE processing each document.
		 * This ensures we don't exceed the budget, with some tolerance
		 * for the size of a single document.
		 */
		if (MemoryContextMemAllocated(build_state.active->mcxt, true) >=
			shared->memory_budget_per_worker)
		{
			TpLocalMemtable *to_spill;
			TpLocalMemtable *alternate;

			/*
			 * Double-buffering: swap to alternate memtable and spill.
			 * If alternate is also full (edge case with very large documents),
			 * we must spill it first before continuing.
			 */
			alternate = tp_worker_state_get_alternate(&build_state);
			if (alternate->num_docs > 0)
			{
				/* Alternate has pending data - spill it first */
				tp_worker_spill_memtable(
						alternate, index, shared, worker_id, my_info);
				tp_local_memtable_clear(alternate);
			}

			/* Swap: new active is the cleared alternate */
			to_spill = tp_worker_state_swap(&build_state);

			/* Spill the previous active memtable */
			tp_worker_spill_memtable(
					to_spill, index, shared, worker_id, my_info);
			tp_local_memtable_clear(to_spill);
			build_state.spilling = NULL;
		}

		tp_worker_process_document(
				build_state.active,
				slot,
				shared->attnum,
				shared->text_config_oid);

		tuples_processed++;
		pg_atomic_fetch_add_u64(&shared->tuples_scanned, 1);

		CHECK_FOR_INTERRUPTS();
	}

	/* Final spill of remaining data from both memtables */
	if (build_state.memtable_a->num_docs > 0)
	{
		tp_worker_spill_memtable(
				build_state.memtable_a, index, shared, worker_id, my_info);
	}
	if (build_state.memtable_b->num_docs > 0)
	{
		tp_worker_spill_memtable(
				build_state.memtable_b, index, shared, worker_id, my_info);
	}

	/* Update global stats */
	pg_atomic_fetch_add_u64(&shared->total_docs, my_info->docs_indexed);
	pg_atomic_fetch_add_u64(&shared->total_len, my_info->total_len);

	/* Signal completion */
	SpinLockAcquire(&shared->mutex);
	shared->workers_done++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->workersdonecv);

	elog(DEBUG1,
		 "Worker %d done: %ld tuples, %d segments, %ld docs",
		 worker_id,
		 (long)tuples_processed,
		 my_info->segment_count,
		 (long)my_info->docs_indexed);

	/* Cleanup */
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	tp_worker_state_destroy(&build_state);
	index_close(index, RowExclusiveLock);
	table_close(heap, AccessShareLock);
}

/*
 * Leader participates as worker 0
 *
 * Uses the same double-buffering approach as regular workers.
 */
static void
tp_leader_participate(
		TpParallelBuildShared *shared,
		Relation			   heap,
		Relation			   index,
		Snapshot			   snapshot)
{
	TpWorkerSegmentInfo *my_info;
	TableScanDesc		 scan;
	TupleTableSlot		*slot;
	TpWorkerBuildState	 build_state;
	int64				 tuples_processed = 0;
	int					 worker_id		  = 0; /* leader is worker 0 */

	(void)snapshot; /* unused but part of interface */

	my_info = &TpParallelWorkerInfo(shared)[worker_id];

	elog(DEBUG1,
		 "Leader participating as worker %d, attnum=%d",
		 worker_id,
		 shared->attnum);

	/* Enable parallel build mode - disables FSM for page allocation */
	tp_set_parallel_build_mode(true);

	/* Initialize double-buffered worker state */
	tp_worker_state_init(&build_state);

	/* Join parallel table scan */
	scan = table_beginscan_parallel(heap, TpParallelTableScan(shared));
	slot = table_slot_create(heap, NULL);

	elog(DEBUG1, "Leader entering scan loop, attnum=%d", shared->attnum);

	/* Process tuples */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		if (tuples_processed == 0)
			elog(DEBUG1, "Leader processing first tuple");

		/*
		 * Check memory budget BEFORE processing each document.
		 */
		if (MemoryContextMemAllocated(build_state.active->mcxt, true) >=
			shared->memory_budget_per_worker)
		{
			TpLocalMemtable *to_spill;
			TpLocalMemtable *alternate;

			/* Double-buffering: swap and spill */
			alternate = tp_worker_state_get_alternate(&build_state);
			if (alternate->num_docs > 0)
			{
				tp_worker_spill_memtable(
						alternate, index, shared, worker_id, my_info);
				tp_local_memtable_clear(alternate);
			}

			to_spill = tp_worker_state_swap(&build_state);
			tp_worker_spill_memtable(
					to_spill, index, shared, worker_id, my_info);
			tp_local_memtable_clear(to_spill);
			build_state.spilling = NULL;
		}

		tp_worker_process_document(
				build_state.active,
				slot,
				shared->attnum,
				shared->text_config_oid);

		tuples_processed++;
		if (tuples_processed % 100000 == 0)
			elog(DEBUG1, "Leader: %ld tuples processed", tuples_processed);
		pg_atomic_fetch_add_u64(&shared->tuples_scanned, 1);

		CHECK_FOR_INTERRUPTS();
	}

	/* Final spill of remaining data from both memtables */
	if (build_state.memtable_a->num_docs > 0)
	{
		tp_worker_spill_memtable(
				build_state.memtable_a, index, shared, worker_id, my_info);
	}
	if (build_state.memtable_b->num_docs > 0)
	{
		tp_worker_spill_memtable(
				build_state.memtable_b, index, shared, worker_id, my_info);
	}

	/* Update global stats */
	pg_atomic_fetch_add_u64(&shared->total_docs, my_info->docs_indexed);
	pg_atomic_fetch_add_u64(&shared->total_len, my_info->total_len);

	/* Signal completion */
	SpinLockAcquire(&shared->mutex);
	shared->workers_done++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->workersdonecv);

	elog(DEBUG1,
		 "Leader done: %ld tuples, %d segments, %ld docs",
		 (long)tuples_processed,
		 my_info->segment_count,
		 (long)my_info->docs_indexed);

	/* Cleanup */
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	tp_worker_state_destroy(&build_state);
}

/*
 * Process a single document tuple into local memtable
 */
static void
tp_worker_process_document(
		TpLocalMemtable *memtable,
		TupleTableSlot	*slot,
		int				 attnum,
		Oid				 text_config_oid)
{
	bool		isnull;
	Datum		text_datum;
	text	   *document_text;
	ItemPointer ctid;
	char	   *document_str;
	Datum		tsvector_datum;
	TSVector	tsvector;
	int			i;
	int			doc_length = 0;
	WordEntry  *we;

	/* Get text value */
	text_datum = slot_getattr(slot, attnum, &isnull);
	if (isnull)
		return;

	/*
	 * Note: DatumGetTextP may detoast, which can crash if text_datum
	 * is invalid. The attnum must match the actual column position.
	 */
	document_text = DatumGetTextP(text_datum);
	ctid		  = &slot->tts_tid;

	if (!ItemPointerIsValid(ctid))
		return;

	/* Tokenize document */
	document_str   = text_to_cstring(document_text);
	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid,
			ObjectIdGetDatum(text_config_oid),
			PointerGetDatum(document_text));
	tsvector = DatumGetTSVector(tsvector_datum);

	if (tsvector->size == 0)
	{
		pfree(document_str);
		return;
	}

	we = ARRPTR(tsvector);

	/* Add each term to local memtable */
	for (i = 0; i < tsvector->size; i++)
	{
		char *lexeme_start = STRPTR(tsvector) + we[i].pos;
		int	  lexeme_len   = we[i].len;
		int32 frequency;

		if (we[i].haspos)
			frequency = (int32)POSDATALEN(tsvector, &we[i]);
		else
			frequency = 1;

		tp_local_memtable_add_term(
				memtable, lexeme_start, lexeme_len, ctid, frequency);

		doc_length += frequency;
	}

	/* Store document length */
	tp_local_memtable_store_doc_length(memtable, ctid, doc_length);

	pfree(document_str);
}

/*
 * Per-term block information for streaming format
 */
typedef struct LocalTermBlockInfo
{
	uint32 posting_offset;	 /* Absolute offset where postings were written */
	uint16 block_count;		 /* Number of blocks for this term */
	uint32 doc_freq;		 /* Document frequency */
	uint32 skip_entry_start; /* Index into accumulated skip entries array */
} LocalTermBlockInfo;

/*
 * Write segment from local memtable
 * Returns the block number of the segment header, or InvalidBlockNumber
 *
 * Layout: [header] → [dictionary] → [postings] → [skip index] →
 *         [fieldnorm] → [ctid map]
 */
static BlockNumber
tp_write_segment_from_local_memtable(
		TpLocalMemtable		  *memtable,
		Relation			   index,
		TpParallelBuildShared *shared,
		int					   worker_id)
{
	TpSegmentWriter		writer;
	TpSegmentHeader		header;
	TpDocMapBuilder	   *docmap;
	TpLocalPosting	  **sorted_terms;
	int					num_terms;
	BlockNumber			header_block;
	uint32			   *string_offsets;
	uint32				string_pos;
	int					i;
	LocalTermBlockInfo *term_blocks;

	/* Accumulated skip entries for all terms */
	TpSkipEntry *all_skip_entries;
	uint32		 skip_entries_count;
	uint32		 skip_entries_capacity;

	/* Skip if nothing to write */
	if (memtable->num_terms == 0 || memtable->num_docs == 0)
		return InvalidBlockNumber;

	/* Build docmap from local memtable */
	docmap = tp_docmap_create();
	tp_local_memtable_foreach_doc(memtable, docmap_add_callback, docmap);
	tp_docmap_finalize(docmap);

	/* Get sorted terms */
	sorted_terms = tp_local_memtable_get_sorted_terms(memtable, &num_terms);
	if (num_terms == 0)
	{
		tp_docmap_destroy(docmap);
		pfree(sorted_terms);
		return InvalidBlockNumber;
	}

	/* Initialize writer with shared page pool */
	tp_segment_writer_init_with_pool(
			&writer,
			index,
			TpParallelPagePool(shared),
			shared->total_pool_pages,
			&shared->shared_pool_next);

	header_block = writer.pages[0];

	/* Initialize header */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_VERSION;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= 0;
	header.num_terms	= num_terms;
	header.level		= 0;
	header.next_segment = InvalidBlockNumber;
	header.num_docs		= memtable->num_docs;
	header.total_tokens = memtable->total_len;

	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Write placeholder header */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary num_terms and string offsets */
	tp_segment_writer_write(&writer, &num_terms, sizeof(uint32));

	/* Calculate and write string offsets */
	string_offsets = palloc0(num_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + sorted_terms[i]->term_len +
					  sizeof(uint32);
	}
	tp_segment_writer_write(
			&writer, string_offsets, num_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = writer.current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = sorted_terms[i]->term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, sorted_terms[i]->term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Record entries offset */
	header.entries_offset = writer.current_offset;

	/* Write placeholder dict entries */
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
	term_blocks = palloc0(num_terms * sizeof(LocalTermBlockInfo));

	skip_entries_capacity = 1024;
	skip_entries_count	  = 0;
	all_skip_entries = palloc(skip_entries_capacity * sizeof(TpSkipEntry));

	/*
	 * Streaming pass: for each term, convert postings and write immediately.
	 */
	for (i = 0; i < num_terms; i++)
	{
		TpLocalPosting		*posting = sorted_terms[i];
		uint32				 doc_count;
		uint32				 num_blocks;
		uint32				 block_idx;
		TpBlockPosting		*block_postings;
		TpLocalPostingEntry *entries;
		uint32				 j;

		/* Record where this term's postings start */
		term_blocks[i].posting_offset	= writer.current_offset;
		term_blocks[i].skip_entry_start = skip_entries_count;

		doc_count = posting->doc_count;
		entries	  = posting->entries;

		/* doc_freq equals doc_count for a term's posting list */
		term_blocks[i].doc_freq = doc_count;

		if (doc_count == 0)
		{
			term_blocks[i].block_count = 0;
			continue;
		}

		/* Calculate number of blocks */
		num_blocks = (doc_count + TP_BLOCK_SIZE - 1) / TP_BLOCK_SIZE;
		if (num_blocks == 0 && doc_count > 0)
			num_blocks = 1;
		term_blocks[i].block_count = (uint16)num_blocks;

		/* Convert postings to block format */
		block_postings = palloc(doc_count * sizeof(TpBlockPosting));
		for (j = 0; j < doc_count; j++)
		{
			uint32 doc_id;
			uint8  norm;

			doc_id = tp_docmap_lookup(docmap, &entries[j].ctid);

			if (doc_id == UINT32_MAX)
				elog(ERROR,
					 "CTID (%u,%u) not found in docmap",
					 ItemPointerGetBlockNumber(&entries[j].ctid),
					 ItemPointerGetOffsetNumber(&entries[j].ctid));

			norm = tp_docmap_get_fieldnorm(docmap, doc_id);

			block_postings[j].doc_id	= doc_id;
			block_postings[j].frequency = (uint16)entries[j].frequency;
			block_postings[j].fieldnorm = norm;
			block_postings[j].reserved	= 0;
		}

		/* Write posting blocks and build skip entries */
		for (block_idx = 0; block_idx < num_blocks; block_idx++)
		{
			TpSkipEntry skip;
			uint32		block_start = block_idx * TP_BLOCK_SIZE;
			uint32 block_end   = Min(block_start + TP_BLOCK_SIZE, doc_count);
			uint16 max_tf	   = 0;
			uint8  max_norm	   = 0;
			uint32 last_doc_id = 0;

			/* Calculate block stats */
			for (j = block_start; j < block_end; j++)
			{
				if (block_postings[j].doc_id > last_doc_id)
					last_doc_id = block_postings[j].doc_id;
				if (block_postings[j].frequency > max_tf)
					max_tf = block_postings[j].frequency;
				if (block_postings[j].fieldnorm > max_norm)
					max_norm = block_postings[j].fieldnorm;
			}

			/* Build skip entry with actual posting offset */
			skip.last_doc_id	= last_doc_id;
			skip.doc_count		= (uint8)(block_end - block_start);
			skip.block_max_tf	= max_tf;
			skip.block_max_norm = max_norm;
			skip.posting_offset = writer.current_offset;
			skip.flags			= TP_BLOCK_FLAG_UNCOMPRESSED;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			/* Accumulate skip entry */
			if (skip_entries_count >= skip_entries_capacity)
			{
				skip_entries_capacity *= 2;
				all_skip_entries = repalloc(
						all_skip_entries,
						skip_entries_capacity * sizeof(TpSkipEntry));
			}
			all_skip_entries[skip_entries_count++] = skip;

			/* Write posting block data */
			tp_segment_writer_write(
					&writer,
					&block_postings[block_start],
					(block_end - block_start) * sizeof(TpBlockPosting));
		}

		pfree(block_postings);
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

	/* Fieldnorm table */
	header.fieldnorm_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer, docmap->fieldnorms, docmap->num_docs * sizeof(uint8));
	}

	/* CTID pages array */
	header.ctid_pages_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_pages,
				docmap->num_docs * sizeof(BlockNumber));
	}

	/* CTID offsets array */
	header.ctid_offsets_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_offsets,
				docmap->num_docs * sizeof(OffsetNumber));
	}

	/* Flush and record page count */
	tp_segment_writer_flush(&writer);
	header.num_pages = writer.pages_allocated;
	header.data_size = writer.current_offset;

	/*
	 * Mark buffer as empty to prevent tp_segment_writer_finish from flushing
	 * again and overwriting our dict entry updates.
	 */
	writer.buffer_pos = SizeOfPageHeaderData;

	/* Write page index using pool (parallel-safe) */
	header.page_index = write_page_index_from_pool(
			index,
			writer.pages,
			writer.pages_allocated,
			TpParallelPagePool(shared),
			shared->total_pool_pages,
			&shared->shared_pool_next);

	/*
	 * Now write the dictionary entries with correct skip_index_offset values.
	 * Do this BEFORE tp_segment_writer_finish so writer.pages is still valid.
	 */
	{
		Buffer dict_buf = InvalidBuffer;
		uint32 entry_logical_page;
		uint32 current_page = UINT32_MAX;

		for (i = 0; i < num_terms; i++)
		{
			TpDictEntry entry;
			uint32		entry_offset;
			uint32		page_offset;
			BlockNumber physical_block;

			/* Build the entry */
			entry.skip_index_offset = header.skip_index_offset +
									  (term_blocks[i].skip_entry_start *
									   sizeof(TpSkipEntry));
			entry.block_count = term_blocks[i].block_count;
			entry.reserved	  = 0;
			entry.doc_freq	  = term_blocks[i].doc_freq;

			/* Calculate where this entry is in the segment */
			entry_offset = header.entries_offset + (i * sizeof(TpDictEntry));
			entry_logical_page = entry_offset / SEGMENT_DATA_PER_PAGE;
			page_offset		   = entry_offset % SEGMENT_DATA_PER_PAGE;

			/* Bounds check */
			if (entry_logical_page >= writer.pages_allocated)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("dict entry %d logical page %u >= "
								"pages_allocated %u",
								i,
								entry_logical_page,
								writer.pages_allocated)));

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
					/* Entry spans two pages - write first part */
					Page  page = BufferGetPage(dict_buf);
					char *dest = (char *)page + SizeOfPageHeaderData +
								 page_offset;
					memcpy(dest, &entry, bytes_on_this_page);

					/* Unlock first page, get second */
					MarkBufferDirty(dict_buf);
					UnlockReleaseBuffer(dict_buf);

					physical_block = writer.pages[entry_logical_page + 1];
					dict_buf	   = ReadBuffer(index, physical_block);
					LockBuffer(dict_buf, BUFFER_LOCK_EXCLUSIVE);
					current_page = entry_logical_page + 1;

					/* Write second part at start of next page */
					page = BufferGetPage(dict_buf);
					dest = (char *)page + SizeOfPageHeaderData;
					memcpy(dest,
						   ((char *)&entry) + bytes_on_this_page,
						   sizeof(TpDictEntry) - bytes_on_this_page);
				}
			}
		}

		/* Release last page if we held one */
		if (current_page != UINT32_MAX)
		{
			MarkBufferDirty(dict_buf);
			UnlockReleaseBuffer(dict_buf);
		}
	}

	/* Write final header */
	{
		Buffer buf;
		Page   page;

		buf = ReadBuffer(index, header_block);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		memcpy((char *)page + SizeOfPageHeaderData,
			   &header,
			   sizeof(TpSegmentHeader));
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
	}
	tp_segment_writer_finish(&writer);

	elog(DEBUG1,
		 "Worker %d: segment complete, header_block=%u, %d terms",
		 worker_id,
		 header_block,
		 num_terms);

	/* Cleanup */
	if (writer.pages)
		pfree(writer.pages);
	pfree(term_blocks);
	pfree(string_offsets);
	pfree(sorted_terms);
	tp_docmap_destroy(docmap);

	return header_block;
}

/*
 * Link all worker segment chains into L0
 */
static void
tp_link_all_worker_segments(TpParallelBuildShared *shared, Relation index)
{
	TpWorkerSegmentInfo *worker_info;
	Buffer				 metabuf;
	Page				 metapage;
	TpIndexMetaPage		 metap;
	BlockNumber			 chain_head		= InvalidBlockNumber;
	BlockNumber			 chain_tail		= InvalidBlockNumber;
	int					 total_segments = 0;
	int					 i;

	worker_info = TpParallelWorkerInfo(shared);

	/* Chain all worker segments together */
	for (i = 0; i < shared->worker_count; i++)
	{
		if (worker_info[i].segment_head == InvalidBlockNumber)
			continue;

		if (chain_head == InvalidBlockNumber)
		{
			chain_head = worker_info[i].segment_head;
			chain_tail = worker_info[i].segment_tail;
		}
		else
		{
			/* Link chain_tail to this worker's head */
			Buffer			 tail_buf;
			Page			 tail_page;
			TpSegmentHeader *tail_header;

			tail_buf = ReadBuffer(index, chain_tail);
			LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
			tail_page				  = BufferGetPage(tail_buf);
			tail_header				  = (TpSegmentHeader *)((char *)tail_page +
												SizeOfPageHeaderData);
			tail_header->next_segment = worker_info[i].segment_head;
			MarkBufferDirty(tail_buf);
			UnlockReleaseBuffer(tail_buf);

			chain_tail = worker_info[i].segment_tail;
		}

		total_segments += worker_info[i].segment_count;
	}

	/* Update metapage */
	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	metap->level_heads[0]  = chain_head;
	metap->level_counts[0] = total_segments;

	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	elog(DEBUG1,
		 "Linked %d segments from %d workers into L0",
		 total_segments,
		 shared->worker_count);

	/*
	 * Check if compaction is needed based on segment count threshold.
	 *
	 * Parallel builds create multiple segments (one per worker, potentially
	 * more with spills). We use the standard threshold-based compaction to
	 * ensure parallel builds produce the same index structure as serial
	 * builds with the same segments_per_level setting.
	 */
	tp_maybe_compact_level(index, 0);
}

/*
 * Finalize statistics in metapage
 */
static void
tp_finalize_parallel_stats(TpParallelBuildShared *shared, Relation index)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	uint64			total_docs;
	uint64			total_len;

	total_docs = pg_atomic_read_u64(&shared->total_docs);
	total_len  = pg_atomic_read_u64(&shared->total_len);

	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	metap->total_docs = (int32)total_docs;
	metap->total_len  = (int64)total_len;
	/* IDF sum calculated lazily on first query */

	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	elog(DEBUG1,
		 "Parallel build complete: %lu docs, %lu total length",
		 (unsigned long)total_docs,
		 (unsigned long)total_len);
}

/*
 * Get a page from the shared pool (called from segment writer)
 * Falls back to FSM/extend if pool exhausted
 *
 * Note: worker_id parameter is kept for API compatibility but no longer used
 * since all workers share a single pool.
 */
BlockNumber
tp_pool_get_page(
		TpParallelBuildShared *shared,
		int					   worker_id __attribute__((unused)),
		Relation			   index)
{
	BlockNumber *pool;
	uint32		 idx;
	BlockNumber	 block;

	pool = TpParallelPagePool(shared);
	idx	 = pg_atomic_fetch_add_u32(&shared->shared_pool_next, 1);

	if (idx < (uint32)shared->total_pool_pages)
	{
		block = pool[idx];

		/* Track highest block used for potential truncation */
		{
			uint32 current_max;
			do
			{
				current_max = pg_atomic_read_u32(&shared->max_block_used);
				if (block <= current_max)
					break;
			} while (!pg_atomic_compare_exchange_u32(
					&shared->max_block_used, &current_max, block));
		}

		return block;
	}

	/* Pool exhausted */
	pg_atomic_write_u32(&shared->pool_exhausted, 1);

	elog(ERROR,
		 "Parallel build page pool exhausted (used all %d pages). "
		 "Increase TP_INDEX_EXPANSION_FACTOR to fix this issue.",
		 shared->total_pool_pages);

	/* Not reached */
	return InvalidBlockNumber;
}
