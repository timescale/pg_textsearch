/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
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

/* GUC: expansion factor for page pool estimation
 * (pg_textsearch.parallel_build_expansion_factor) */
extern double tp_parallel_build_expansion_factor;

/*
 * Worker build state - single memtable that gets spilled when full.
 */
typedef struct TpWorkerBuildState
{
	TpLocalMemtable *memtable; /* Active memtable being filled */
} TpWorkerBuildState;

/*
 * Initialize worker state
 */
static void
tp_worker_state_init(TpWorkerBuildState *state)
{
	state->memtable = tp_local_memtable_create();
}

/*
 * Destroy worker state
 */
static void
tp_worker_state_destroy(TpWorkerBuildState *state)
{
	if (state->memtable)
		tp_local_memtable_destroy(state->memtable);
	state->memtable = NULL;
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
 * Estimate total pages needed for index build page pool.
 *
 * Includes data pages (based on heap size * expansion factor) plus
 * estimated page index pages for multiple segments per worker.
 */
static int
estimate_parallel_pool_pages(BlockNumber heap_pages, int nworkers)
{
	int data_pages;
	int entries_per_page;
	int page_index_pages;
	int estimated_segments;

	data_pages = (int)(heap_pages * tp_parallel_build_expansion_factor) +
				 TP_MIN_PAGES_PER_WORKER * (nworkers + 1);
	entries_per_page   = tp_page_index_entries_per_page();
	estimated_segments = (nworkers + 1) * 10; /* ~10 segments per worker */

	/* Each segment needs at least 1 page index page, plus data page mapping */
	page_index_pages = estimated_segments +
					   (data_pages + entries_per_page - 1) / entries_per_page;

	return data_pages + page_index_pages;
}

/*
 * Truncate unused pages from pre-allocated pool.
 *
 * Pool pages are allocated contiguously, so unused pages are at the end.
 * Truncation reclaims this space immediately.
 */
static void
truncate_unused_pool_pages(Relation index, TpParallelBuildShared *shared)
{
	uint32		 pool_used	= pg_atomic_read_u32(&shared->shared_pool_next);
	uint32		 pool_total = shared->total_pool_pages;
	BlockNumber *pool		= TpParallelPagePool(shared);

	if (pool_used < pool_total && pool_used > 0)
	{
		BlockNumber truncate_to = pool[0] + pool_used;
		BlockNumber old_nblocks = RelationGetNumberOfBlocks(index);
		ForkNumber	forknum		= MAIN_FORKNUM;

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
		CacheInvalidateRelcache(index);
	}
	else if (pool_used == 0)
	{
		elog(WARNING, "Parallel build used 0 pool pages - no data?");
	}
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
	int					   launched;

	/* Ensure reasonable number of workers */
	if (nworkers > TP_MAX_PARALLEL_WORKERS)
		nworkers = TP_MAX_PARALLEL_WORKERS;

	total_pool_pages = estimate_parallel_pool_pages(
			RelationGetNumberOfBlocks(heap), nworkers);

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

	/*
	 * Leader always participates as a worker.
	 * Even if no background workers were launched, the leader will scan
	 * the entire table as worker 0.
	 */
	shared->leader_working = true;
	tp_leader_participate(shared, heap, index, snapshot);

	/* Wait for all workers to finish */
	WaitForParallelWorkersToFinish(pcxt);

	/* Reclaim unused pages from the pre-allocated pool */
	truncate_unused_pool_pages(index, shared);

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

	/*
	 * Per-worker memory budget calculation.
	 *
	 * We split maintenance_work_mem evenly across workers.
	 * We use 90% of the budget as the actual threshold to provide slop
	 * and avoid thrashing near the boundary.
	 */
#define TP_MEMORY_SLOP_FACTOR 0.9

	{
		Size memory_budget;
		int	 total_workers = nworkers + 1; /* background workers + leader */

		/* maintenance_work_mem is in KB, convert to bytes */
		memory_budget = ((Size)maintenance_work_mem * 1024) / total_workers;

		/* Apply slop factor to avoid thrashing near boundary */
		shared->memory_budget_per_worker = (Size)(memory_budget *
												  TP_MEMORY_SLOP_FACTOR);
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
 * Each worker scans a portion of the heap, builds a local memtable,
 * and spills to segments when the memory budget is exceeded.
 */
PGDLLEXPORT void
tp_parallel_build_worker_main(
		dsm_segment *seg __attribute__((unused)), shm_toc *toc)
{
	TpParallelBuildShared *shared;
	TpWorkerSegmentInfo	  *my_info;
	Relation			   heap;
	Relation			   index;
	TableScanDesc		   scan;
	TupleTableSlot		  *slot;
	TpWorkerBuildState	   build_state;
	int					   worker_id;

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
		if (MemoryContextMemAllocated(build_state.memtable->mcxt, true) >=
			shared->memory_budget_per_worker)
		{
			/* Spill memtable to segment and clear for reuse */
			tp_worker_spill_memtable(
					build_state.memtable, index, shared, worker_id, my_info);
			tp_local_memtable_clear(build_state.memtable);
		}

		tp_worker_process_document(
				build_state.memtable,
				slot,
				shared->attnum,
				shared->text_config_oid);

		pg_atomic_fetch_add_u64(&shared->tuples_scanned, 1);

		CHECK_FOR_INTERRUPTS();
	}

	/* Final spill of any remaining data */
	if (build_state.memtable->num_docs > 0)
	{
		tp_worker_spill_memtable(
				build_state.memtable, index, shared, worker_id, my_info);
	}

	/* Update global stats */
	pg_atomic_fetch_add_u64(&shared->total_docs, my_info->docs_indexed);
	pg_atomic_fetch_add_u64(&shared->total_len, my_info->total_len);

	/* Signal completion */
	SpinLockAcquire(&shared->mutex);
	shared->workers_done++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->workersdonecv);

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
	int					 worker_id = 0; /* leader is worker 0 */

	(void)snapshot; /* unused but part of interface */

	my_info = &TpParallelWorkerInfo(shared)[worker_id];

	/* Initialize worker state */
	tp_worker_state_init(&build_state);

	/* Join parallel table scan */
	scan = table_beginscan_parallel(heap, TpParallelTableScan(shared));
	slot = table_slot_create(heap, NULL);

	/* Process tuples */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		/*
		 * Check memory budget BEFORE processing each document.
		 */
		if (MemoryContextMemAllocated(build_state.memtable->mcxt, true) >=
			shared->memory_budget_per_worker)
		{
			/* Spill memtable to segment and clear for reuse */
			tp_worker_spill_memtable(
					build_state.memtable, index, shared, worker_id, my_info);
			tp_local_memtable_clear(build_state.memtable);
		}

		tp_worker_process_document(
				build_state.memtable,
				slot,
				shared->attnum,
				shared->text_config_oid);

		pg_atomic_fetch_add_u64(&shared->tuples_scanned, 1);

		CHECK_FOR_INTERRUPTS();
	}

	/* Final spill of any remaining data */
	if (build_state.memtable->num_docs > 0)
	{
		tp_worker_spill_memtable(
				build_state.memtable, index, shared, worker_id, my_info);
	}

	/* Update global stats */
	pg_atomic_fetch_add_u64(&shared->total_docs, my_info->docs_indexed);
	pg_atomic_fetch_add_u64(&shared->total_len, my_info->total_len);

	/* Signal completion */
	SpinLockAcquire(&shared->mutex);
	shared->workers_done++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->workersdonecv);

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
 * Accumulated skip entries during posting write.
 */
typedef struct SkipEntryAccumulator
{
	TpSkipEntry *entries;
	uint32		 count;
	uint32		 capacity;
} SkipEntryAccumulator;

/*
 * Initialize skip entry accumulator.
 */
static void
skip_accum_init(SkipEntryAccumulator *accum)
{
	accum->capacity = 1024;
	accum->count	= 0;
	accum->entries	= palloc(accum->capacity * sizeof(TpSkipEntry));
}

/*
 * Add a skip entry to the accumulator.
 */
static void
skip_accum_add(SkipEntryAccumulator *accum, TpSkipEntry *entry)
{
	if (accum->count >= accum->capacity)
	{
		accum->capacity *= 2;
		accum->entries = repalloc(
				accum->entries, accum->capacity * sizeof(TpSkipEntry));
	}
	accum->entries[accum->count++] = *entry;
}

/*
 * Free skip entry accumulator.
 */
static void
skip_accum_destroy(SkipEntryAccumulator *accum)
{
	pfree(accum->entries);
}

/*
 * Write postings for a single term and accumulate skip entries.
 * Returns number of blocks written for this term.
 */
static uint16
write_term_postings(
		TpSegmentWriter		 *writer,
		TpLocalPosting		 *posting,
		TpDocMapBuilder		 *docmap,
		LocalTermBlockInfo	 *term_info,
		SkipEntryAccumulator *skip_accum)
{
	uint32			doc_count = posting->doc_count;
	uint32			num_blocks;
	uint32			block_idx;
	TpBlockPosting *block_postings;
	TpPostingEntry *entries = posting->entries;
	uint32			j;

	/* Record where this term's postings start */
	term_info->posting_offset	= writer->current_offset;
	term_info->skip_entry_start = skip_accum->count;
	term_info->doc_freq			= doc_count;

	if (doc_count == 0)
	{
		term_info->block_count = 0;
		return 0;
	}

	/* Calculate number of blocks */
	num_blocks = (doc_count + TP_BLOCK_SIZE - 1) / TP_BLOCK_SIZE;
	if (num_blocks == 0 && doc_count > 0)
		num_blocks = 1;
	term_info->block_count = (uint16)num_blocks;

	/* Convert postings to block format */
	block_postings = palloc(doc_count * sizeof(TpBlockPosting));
	for (j = 0; j < doc_count; j++)
	{
		uint32 doc_id = tp_docmap_lookup(docmap, &entries[j].ctid);
		if (doc_id == UINT32_MAX)
			elog(ERROR,
				 "CTID (%u,%u) not found in docmap",
				 ItemPointerGetBlockNumber(&entries[j].ctid),
				 ItemPointerGetOffsetNumber(&entries[j].ctid));

		block_postings[j].doc_id	= doc_id;
		block_postings[j].frequency = (uint16)entries[j].frequency;
		block_postings[j].fieldnorm = tp_docmap_get_fieldnorm(docmap, doc_id);
		block_postings[j].reserved	= 0;
	}

	/* Write posting blocks and build skip entries */
	for (block_idx = 0; block_idx < num_blocks; block_idx++)
	{
		TpSkipEntry skip;
		uint32		block_start = block_idx * TP_BLOCK_SIZE;
		uint32		block_end	= Min(block_start + TP_BLOCK_SIZE, doc_count);
		uint16		max_tf		= 0;
		uint8		max_norm	= 0;
		uint32		last_doc_id = 0;

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

		/* Build skip entry */
		skip.last_doc_id	= last_doc_id;
		skip.doc_count		= (uint8)(block_end - block_start);
		skip.block_max_tf	= max_tf;
		skip.block_max_norm = max_norm;
		skip.posting_offset = writer->current_offset;
		skip.flags			= TP_BLOCK_FLAG_UNCOMPRESSED;
		memset(skip.reserved, 0, sizeof(skip.reserved));

		skip_accum_add(skip_accum, &skip);

		/* Write posting block data */
		tp_segment_writer_write(
				writer,
				&block_postings[block_start],
				(block_end - block_start) * sizeof(TpBlockPosting));
	}

	pfree(block_postings);
	return (uint16)num_blocks;
}

/*
 * Update dictionary entries with final skip index offsets.
 * Must be called after postings and skip entries are written.
 */
static void
update_dict_entries(
		TpSegmentWriter	   *writer,
		Relation			index,
		TpSegmentHeader	   *header,
		LocalTermBlockInfo *term_blocks,
		int					num_terms)
{
	Buffer dict_buf		= InvalidBuffer;
	uint32 current_page = UINT32_MAX;
	int	   i;

	for (i = 0; i < num_terms; i++)
	{
		TpDictEntry entry;
		uint32		entry_offset;
		uint32		entry_logical_page;
		uint32		page_offset;
		BlockNumber physical_block;

		/* Build the entry */
		entry.skip_index_offset = header->skip_index_offset +
								  (term_blocks[i].skip_entry_start *
								   sizeof(TpSkipEntry));
		entry.block_count = term_blocks[i].block_count;
		entry.reserved	  = 0;
		entry.doc_freq	  = term_blocks[i].doc_freq;

		/* Calculate where this entry is in the segment */
		entry_offset = header->entries_offset + (i * sizeof(TpDictEntry));
		entry_logical_page = entry_offset / SEGMENT_DATA_PER_PAGE;
		page_offset		   = entry_offset % SEGMENT_DATA_PER_PAGE;

		/* Bounds check */
		if (entry_logical_page >= writer->pages_allocated)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("dict entry %d logical page %u >= pages_allocated "
							"%u",
							i,
							entry_logical_page,
							writer->pages_allocated)));

		/* Read page if different from current */
		if (entry_logical_page != current_page)
		{
			if (current_page != UINT32_MAX)
			{
				MarkBufferDirty(dict_buf);
				UnlockReleaseBuffer(dict_buf);
			}

			physical_block = writer->pages[entry_logical_page];
			dict_buf	   = ReadBuffer(index, physical_block);
			LockBuffer(dict_buf, BUFFER_LOCK_EXCLUSIVE);
			current_page = entry_logical_page;
		}

		/* Write entry to page */
		{
			uint32 bytes_on_this_page = SEGMENT_DATA_PER_PAGE - page_offset;

			if (bytes_on_this_page >= sizeof(TpDictEntry))
			{
				/* Entry fits entirely on this page */
				Page  page = BufferGetPage(dict_buf);
				char *dest = (char *)page + SizeOfPageHeaderData + page_offset;
				memcpy(dest, &entry, sizeof(TpDictEntry));
			}
			else
			{
				/* Entry spans two pages */
				Page  page = BufferGetPage(dict_buf);
				char *dest = (char *)page + SizeOfPageHeaderData + page_offset;
				memcpy(dest, &entry, bytes_on_this_page);

				MarkBufferDirty(dict_buf);
				UnlockReleaseBuffer(dict_buf);

				physical_block = writer->pages[entry_logical_page + 1];
				dict_buf	   = ReadBuffer(index, physical_block);
				LockBuffer(dict_buf, BUFFER_LOCK_EXCLUSIVE);
				current_page = entry_logical_page + 1;

				page = BufferGetPage(dict_buf);
				dest = (char *)page + SizeOfPageHeaderData;
				memcpy(dest,
					   ((char *)&entry) + bytes_on_this_page,
					   sizeof(TpDictEntry) - bytes_on_this_page);
			}
		}
	}

	/* Release last page */
	if (current_page != UINT32_MAX)
	{
		MarkBufferDirty(dict_buf);
		UnlockReleaseBuffer(dict_buf);
	}
}

/*
 * Write final header to segment.
 */
static void
write_final_header(
		Relation index, BlockNumber header_block, TpSegmentHeader *header)
{
	Buffer buf = ReadBuffer(index, header_block);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	memcpy((char *)BufferGetPage(buf) + SizeOfPageHeaderData,
		   header,
		   sizeof(TpSegmentHeader));
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
}

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
		int					   worker_id __attribute__((unused)))
{
	TpSegmentWriter		 writer;
	TpSegmentHeader		 header;
	TpDocMapBuilder		*docmap;
	TpLocalTermPosting	*sorted_terms;
	int					 num_terms;
	BlockNumber			 header_block;
	uint32				*string_offsets;
	uint32				 string_pos;
	int					 i;
	LocalTermBlockInfo	*term_blocks;
	SkipEntryAccumulator skip_accum;

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
	header.magic			 = TP_SEGMENT_MAGIC;
	header.version			 = TP_SEGMENT_FORMAT_VERSION;
	header.created_at		 = GetCurrentTimestamp();
	header.num_pages		 = 0;
	header.num_terms		 = num_terms;
	header.level			 = 0;
	header.next_segment		 = InvalidBlockNumber;
	header.num_docs			 = memtable->num_docs;
	header.total_tokens		 = memtable->total_len;
	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Write placeholder header */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary: num_terms and string offsets */
	tp_segment_writer_write(&writer, &num_terms, sizeof(uint32));

	string_offsets = palloc0(num_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + sorted_terms[i].term_len +
					  sizeof(uint32);
	}
	tp_segment_writer_write(
			&writer, string_offsets, num_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = writer.current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = sorted_terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, sorted_terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Write placeholder dict entries */
	header.entries_offset = writer.current_offset;
	{
		TpDictEntry placeholder;
		memset(&placeholder, 0, sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
			tp_segment_writer_write(
					&writer, &placeholder, sizeof(TpDictEntry));
	}

	/* Write postings for all terms */
	header.postings_offset = writer.current_offset;
	term_blocks			   = palloc0(num_terms * sizeof(LocalTermBlockInfo));
	skip_accum_init(&skip_accum);

	for (i = 0; i < num_terms; i++)
		write_term_postings(
				&writer,
				sorted_terms[i].posting,
				docmap,
				&term_blocks[i],
				&skip_accum);

	/* Write accumulated skip entries */
	header.skip_index_offset = writer.current_offset;
	if (skip_accum.count > 0)
		tp_segment_writer_write(
				&writer,
				skip_accum.entries,
				skip_accum.count * sizeof(TpSkipEntry));
	skip_accum_destroy(&skip_accum);

	/* Write docmap tables */
	header.fieldnorm_offset = writer.current_offset;
	if (docmap->num_docs > 0)
		tp_segment_writer_write(
				&writer, docmap->fieldnorms, docmap->num_docs * sizeof(uint8));

	header.ctid_pages_offset = writer.current_offset;
	if (docmap->num_docs > 0)
		tp_segment_writer_write(
				&writer,
				docmap->ctid_pages,
				docmap->num_docs * sizeof(BlockNumber));

	header.ctid_offsets_offset = writer.current_offset;
	if (docmap->num_docs > 0)
		tp_segment_writer_write(
				&writer,
				docmap->ctid_offsets,
				docmap->num_docs * sizeof(OffsetNumber));

	/* Flush and finalize header fields */
	tp_segment_writer_flush(&writer);
	header.num_pages = writer.pages_allocated;
	header.data_size = writer.current_offset;

	/* Prevent double-flush when we update dict entries */
	writer.buffer_pos = SizeOfPageHeaderData;

	/* Write page index */
	header.page_index = write_page_index_from_pool(
			index,
			writer.pages,
			writer.pages_allocated,
			TpParallelPagePool(shared),
			shared->total_pool_pages,
			&shared->shared_pool_next);

	/* Update dictionary entries with skip offsets */
	update_dict_entries(&writer, index, &header, term_blocks, num_terms);

	/* Write final header */
	write_final_header(index, header_block, &header);

	tp_segment_writer_finish(&writer);

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
		Relation			   index __attribute__((unused)))
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
		 "Increase pg_textsearch.parallel_build_expansion_factor (currently "
		 "%.1f) and retry.",
		 shared->total_pool_pages,
		 tp_parallel_build_expansion_factor);

	/* Not reached */
	return InvalidBlockNumber;
}
