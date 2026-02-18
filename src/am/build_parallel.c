/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.c - Parallel index build implementation
 *
 * Architecture:
 * - Each worker scans a partition of the heap and builds a local
 *   TpBuildContext (arena + HTAB) — no shared memory allocations.
 * - When the worker's memory budget fills, it flushes a segment
 *   directly to index pages and chains it locally.
 * - After all workers finish, the leader links worker segment
 *   chains into L0, runs compaction, and truncates dead pages.
 *
 * This eliminates DSA, dshash, double-buffering, and leader-side
 * N-way merging. Workers are fully independent.
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <commands/progress.h>
#include <miscadmin.h>
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
#include "segment/merge.h"
#include "segment/segment.h"
#include "state/metapage.h"

/* Defined in build.c, extracts terms from TSVector */
extern int tp_extract_terms_from_tsvector(
		TSVector tsvector,
		char  ***terms_out,
		int32  **frequencies_out,
		int		*term_count_out);

/*
 * Estimate shared memory needed for parallel build
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

	/* Initialize per-worker results */
	results = TpParallelWorkerResults(shared);
	for (i = 0; i < nworkers; i++)
	{
		results[i].segment_head	  = InvalidBlockNumber;
		results[i].segment_tail	  = InvalidBlockNumber;
		results[i].segment_count  = 0;
		results[i].total_docs	  = 0;
		results[i].total_len	  = 0;
		results[i].tuples_scanned = 0;
	}
}

/*
 * Flush build context to a segment and chain it into the
 * worker's local segment list.
 *
 * The newest segment becomes the new head, with its next_segment
 * pointing to the previous head.
 */
static void
tp_worker_flush_and_chain(
		TpBuildContext *ctx, Relation index, TpParallelWorkerResult *result)
{
	BlockNumber segment_root;

	segment_root = tp_write_segment_from_build_ctx(ctx, index);
	if (segment_root == InvalidBlockNumber)
		return;

	/* Chain: new segment points to old head */
	if (result->segment_head != InvalidBlockNumber)
	{
		Buffer			 seg_buf;
		Page			 seg_page;
		TpSegmentHeader *seg_header;

		seg_buf = ReadBuffer(index, segment_root);
		LockBuffer(seg_buf, BUFFER_LOCK_EXCLUSIVE);
		seg_page   = BufferGetPage(seg_buf);
		seg_header = (TpSegmentHeader *)PageGetContents(seg_page);
		seg_header->next_segment = result->segment_head;
		MarkBufferDirty(seg_buf);
		UnlockReleaseBuffer(seg_buf);
	}
	else
	{
		/* First segment — it's also the tail */
		result->segment_tail = segment_root;
	}

	result->segment_head = segment_root;
	result->segment_count++;
}

/*
 * Worker entry point - called by parallel infrastructure
 *
 * Each worker:
 * 1. Opens heap and index relations
 * 2. Creates a local TpBuildContext with per-worker budget
 * 3. Scans its share of the heap
 * 4. Flushes segments when budget fills
 * 5. Reports results via shared memory
 */
PGDLLEXPORT void
tp_parallel_build_worker_main(
		dsm_segment *seg __attribute__((unused)), shm_toc *toc)
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

	/* Attach to shared memory */
	shared = (TpParallelBuildShared *)
			shm_toc_lookup(toc, TP_PARALLEL_KEY_SHARED, false);

	worker_id = ParallelWorkerNumber;
	my_result = &TpParallelWorkerResults(shared)[worker_id];

	/* Open heap and index relations.
	 * Index uses AccessExclusiveLock matching btree nbtsort.c pattern:
	 * leader already holds this lock, workers inherit it via parallel
	 * context. This ensures proper smgr synchronization for concurrent
	 * relation extension.
	 */
	heap  = table_open(shared->heaprelid, AccessShareLock);
	index = index_open(shared->indexrelid, AccessExclusiveLock);

	/* Join parallel table scan */
	scan = table_beginscan_parallel(heap, TpParallelTableScan(shared));
	slot = table_slot_create(heap, NULL);

	/*
	 * Per-worker memory budget: split maintenance_work_mem across workers.
	 * Minimum 64MB per worker to avoid excessive flushing.
	 */
	budget = (Size)maintenance_work_mem * 1024L / shared->nworkers;
	if (budget < 64L * 1024 * 1024)
		budget = 64L * 1024 * 1024;

	build_ctx = tp_build_context_create(budget);

	/*
	 * Set up page pool for worker.  Workers cannot safely extend the
	 * relation concurrently, so the leader pre-allocates a pool of
	 * pages.  Build a BlockNumber array from the contiguous range.
	 */
	{
		BlockNumber *pool;
		uint32		 i;

		pool = palloc(shared->pool_size * sizeof(BlockNumber));
		for (i = 0; i < shared->pool_size; i++)
			pool[i] = shared->pool_start + i;

		build_ctx->page_pool = pool;
		build_ctx->pool_size = shared->pool_size;
		build_ctx->pool_next = &shared->pool_next;
	}

	/*
	 * Per-document memory context for tokenization temporaries.
	 * Reset after each document to prevent unbounded growth.
	 */
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

		/* Reset per-doc context (frees tsvector, terms) */
		MemoryContextReset(build_tmpctx);

		/* Budget-based flush */
		if (tp_build_context_should_flush(build_ctx))
		{
			my_result->total_docs += build_ctx->num_docs;
			my_result->total_len += build_ctx->total_len;

			tp_worker_flush_and_chain(build_ctx, index, my_result);
			tp_build_context_reset(build_ctx);
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
		my_result->total_docs += build_ctx->num_docs;
		my_result->total_len += build_ctx->total_len;

		tp_worker_flush_and_chain(build_ctx, index, my_result);
	}

	/* Cleanup worker-local resources */
	tp_build_context_destroy(build_ctx);
	MemoryContextDelete(build_tmpctx);

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	index_close(index, AccessExclusiveLock);
	table_close(heap, AccessShareLock);

	/* Signal completion */
	pg_atomic_fetch_add_u32(&shared->workers_done, 1);
	ConditionVariableSignal(&shared->all_done_cv);
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
	int					   launched;
	uint64				   total_docs = 0;
	uint64				   total_len  = 0;
	int32				   total_segs = 0;

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

	/*
	 * Pre-allocate page pool for workers.
	 *
	 * Workers cannot safely extend the relation concurrently,
	 * so the leader pre-extends it.  Estimate: index is ~50%
	 * of heap size, plus some headroom for page index pages.
	 * After build, unused pages are truncated.
	 */
	{
		BlockNumber heap_pages;
		uint32		pool_size;
		BlockNumber pool_start;
		uint32		i;

		heap_pages = RelationGetNumberOfBlocks(heap);
		/*
		 * Generous pool: index is typically 30-70% of heap.
		 * Use 1.0x to be safe; unused pages are truncated.
		 */
		pool_size = (uint32)(heap_pages * 1.0) + 512;

		/* Block 0 is metapage; pool starts at block 1 */
		pool_start = RelationGetNumberOfBlocks(index);

		/* Extend relation to create pool pages */
		for (i = 0; i < pool_size; i++)
		{
			Buffer buf = ReadBufferExtended(
					index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
			Assert(BufferGetBlockNumber(buf) == pool_start + i);
			PageInit(BufferGetPage(buf), BLCKSZ, 0);
			MarkBufferDirty(buf);
			UnlockReleaseBuffer(buf);
		}

		shared->pool_start = pool_start;
		shared->pool_size  = pool_size;
		pg_atomic_init_u32(&shared->pool_next, 0);
	}

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
	 * Wait for workers, polling progress periodically.
	 * Workers atomically increment shared->tuples_done; we
	 * report it so pg_stat_progress_create_index stays live.
	 */
	ConditionVariablePrepareToSleep(&shared->all_done_cv);
	while (pg_atomic_read_u32(&shared->workers_done) < (uint32)launched)
	{
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_DONE,
				(int64)pg_atomic_read_u64(&shared->tuples_done));

		ConditionVariableTimedSleep(
				&shared->all_done_cv, 1000 /* ms */, PG_WAIT_EXTENSION);
	}
	ConditionVariableCancelSleep();
	WaitForParallelWorkersToFinish(pcxt);

	/* Collect results from all workers */
	{
		TpParallelWorkerResult *results;
		int						i;

		results = TpParallelWorkerResults(shared);
		for (i = 0; i < launched; i++)
		{
			total_docs += results[i].total_docs;
			total_len += results[i].total_len;
			total_segs += results[i].segment_count;
		}
	}

	/* Report final tuple count */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_TUPLES_DONE, (int64)total_docs);

	/* Report compacting phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_COMPACTING);

	/*
	 * Link all worker segment chains into L0.
	 *
	 * Each worker has a chain: head -> ... -> tail.
	 * We link them sequentially: worker0 tail -> worker1 head -> ...
	 * Then set metapage level_heads[0] = first worker's head.
	 */
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;
		BlockNumber		l0_head = InvalidBlockNumber;
		BlockNumber		l0_tail = InvalidBlockNumber;
		int				i;

		TpParallelWorkerResult *results = TpParallelWorkerResults(shared);

		for (i = 0; i < launched; i++)
		{
			if (results[i].segment_head == InvalidBlockNumber)
				continue;

			if (l0_head == InvalidBlockNumber)
			{
				/* First non-empty worker */
				l0_head = results[i].segment_head;
				l0_tail = results[i].segment_tail;
			}
			else
			{
				/*
				 * Link previous tail to this worker's head.
				 * tail->next_segment = this worker's head.
				 */
				Buffer			 tail_buf;
				Page			 tail_page;
				TpSegmentHeader *tail_header;

				tail_buf = ReadBuffer(index, l0_tail);
				LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
				tail_page	= BufferGetPage(tail_buf);
				tail_header = (TpSegmentHeader *)PageGetContents(tail_page);
				tail_header->next_segment = results[i].segment_head;
				MarkBufferDirty(tail_buf);
				UnlockReleaseBuffer(tail_buf);

				l0_tail = results[i].segment_tail;
			}
		}

		/* Update metapage */
		metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		metap->level_heads[0]  = l0_head;
		metap->level_counts[0] = total_segs;
		metap->total_docs	   = total_docs;
		metap->total_len	   = total_len;

		MarkBufferDirty(metabuf);
		UnlockReleaseBuffer(metabuf);

		/* Compact all segments into one per level */
		tp_compact_all(index);
	}

	/*
	 * Final truncation: shrink index file to minimum needed size.
	 * After compaction, some L0 segment pages are freed. We find
	 * the highest used block across all live segments and truncate.
	 */
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;
		BlockNumber		max_used = 1; /* At least metapage */
		int				level;

		metabuf	 = ReadBuffer(index, TP_METAPAGE_BLKNO);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		/* Find highest block used by any segment */
		for (level = 0; level < TP_MAX_LEVELS; level++)
		{
			BlockNumber seg = metap->level_heads[level];
			while (seg != InvalidBlockNumber)
			{
				BlockNumber *pages;
				uint32		 num_pages;
				uint32		 j;
				BlockNumber	 next_seg;

				num_pages = tp_segment_collect_pages(index, seg, &pages);

				for (j = 0; j < num_pages; j++)
				{
					if (pages[j] + 1 > max_used)
						max_used = pages[j] + 1;
				}

				/* Get next before freeing */
				{
					TpSegmentReader *reader = tp_segment_open(index, seg);
					next_seg				= reader->header->next_segment;
					tp_segment_close(reader);
				}

				if (pages)
					pfree(pages);

				seg = next_seg;
			}
		}
		ReleaseBuffer(metabuf);

		/* Truncate if we can save space */
		{
			BlockNumber nblocks = RelationGetNumberOfBlocks(index);
			if (max_used < nblocks)
			{
				ForkNumber forknum = MAIN_FORKNUM;
				elog(DEBUG1,
					 "Final truncation: max_used=%u, nblocks=%u,"
					 " saving %u pages",
					 max_used,
					 nblocks,
					 nblocks - max_used);
#if PG_VERSION_NUM >= 180000
				smgrtruncate(
						RelationGetSmgr(index),
						&forknum,
						1,
						&nblocks,
						&max_used);
#else
				smgrtruncate(RelationGetSmgr(index), &forknum, 1, &max_used);
#endif
			}
		}
	}

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
