/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am/build_parallel.c - Parallel index build implementation
 *
 * Architecture:
 * - Workers scan the heap and build memtables in shared DSA memory
 * - Each worker has two memtables (double-buffering) for flow control
 * - Leader doesn't scan - it processes worker memtables and writes segments
 * - All disk I/O is done by the leader, ensuring contiguous page allocation
 *
 * This design avoids fragmentation by having a single writer (the leader)
 * that allocates pages sequentially from the pre-allocated pool.
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <commands/progress.h>
#include <lib/dshash.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/condition_variable.h>
#include <storage/smgr.h>
#include <tsearch/ts_type.h>
#include <utils/builtins.h>
#include <utils/dsa.h>
#include <utils/guc.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/wait_event.h>

#include "am.h"
#include "build_parallel.h"
#include "common/hashfn.h"
#include "constants.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "segment/dictionary.h"
#include "segment/docmap.h"
#include "segment/merge.h"
#include "segment/pagemapper.h"
#include "segment/segment.h"
#include "state/metapage.h"

/* GUC: expansion factor for page pool estimation */
extern double tp_parallel_build_expansion_factor;

/* Minimum pages to pre-allocate */
#define TP_MIN_POOL_PAGES 1024

/* Memory slop factor - use 90% of budget to avoid thrashing */
#define TP_MEMORY_SLOP_FACTOR 0.9

/*
 * Forward declarations
 */
static void tp_init_parallel_shared(
		TpParallelBuildShared *shared,
		Relation			   heap,
		Relation			   index,
		Oid					   text_config_oid,
		AttrNumber			   attnum,
		double				   k1,
		double				   b,
		int					   nworkers,
		dsa_area			  *dsa);

static void
tp_preallocate_page_pool(Relation index, TpParallelBuildShared *shared);

static void tp_leader_process_buffers(
		TpParallelBuildShared *shared, Relation index, dsa_area *dsa);

static BlockNumber tp_write_segment_from_worker_buffer(
		TpParallelBuildShared  *shared,
		TpWorkerMemtableBuffer *buffer,
		Relation				index,
		dsa_area			   *dsa);

static void tp_worker_process_document(
		dsa_area			   *dsa,
		TpWorkerMemtableBuffer *buffer,
		TupleTableSlot		   *slot,
		int						attnum,
		Oid						text_config_oid);

static dshash_table *tp_worker_create_string_table(dsa_area *dsa);
static dshash_table *tp_worker_create_doclength_table(dsa_area *dsa);
static uint32
		   tp_worker_string_hash(const void *key, size_t keysize, void *arg);
static int tp_worker_string_compare(
		const void *a, const void *b, size_t keysize, void *arg);

/*
 * Estimate total pages needed for index build page pool
 */
static int
estimate_pool_pages(BlockNumber heap_pages)
{
	int pages = (int)(heap_pages * tp_parallel_build_expansion_factor);
	return Max(pages, TP_MIN_POOL_PAGES);
}

/*
 * Estimate shared memory needed for parallel build
 */
Size
tp_parallel_build_estimate_shmem(
		Relation heap, Snapshot snapshot, int nworkers, int total_pool_pages)
{
	Size size;

	/* Base shared structure */
	size = MAXALIGN(sizeof(TpParallelBuildShared));

	/* Per-worker state array */
	size = add_size(size, MAXALIGN(sizeof(TpWorkerState) * nworkers));

	/* Page pool */
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
	dsa_area			  *dsa;
	int					   launched;

	/* Ensure reasonable number of workers */
	if (nworkers > TP_MAX_PARALLEL_WORKERS)
		nworkers = TP_MAX_PARALLEL_WORKERS;

	/* Estimate page pool size */
	total_pool_pages = estimate_pool_pages(RelationGetNumberOfBlocks(heap));

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

	/* Create DSA for shared memtable allocations */
	dsa = dsa_create(LWTRANCHE_PARALLEL_HASH_JOIN);
	dsa_pin(dsa);
	dsa_pin_mapping(dsa);

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
			nworkers,
			dsa);
	shared->total_pool_pages = total_pool_pages;

	/* Pre-allocate page pool */
	tp_preallocate_page_pool(index, shared);

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
	 * Leader processes worker memtables and writes segments.
	 * This continues until all workers are done.
	 */
	tp_leader_process_buffers(shared, index, dsa);

	/* Wait for all workers to finish */
	WaitForParallelWorkersToFinish(pcxt);

	/* Truncate unused pool pages */
	{
		uint32		 pool_used = pg_atomic_read_u32(&shared->pool_next);
		BlockNumber *pool	   = TpParallelPagePool(shared);
		BlockNumber	 nblocks   = RelationGetNumberOfBlocks(index);
		BlockNumber	 truncate_to;

		if (pool_used > 0 && pool_used < (uint32)shared->total_pool_pages)
		{
			truncate_to = pool[0] + pool_used;
			if (truncate_to < nblocks)
			{
				ForkNumber forknum = MAIN_FORKNUM;
#if PG_VERSION_NUM >= 180000
				smgrtruncate(
						RelationGetSmgr(index),
						&forknum,
						1,
						&nblocks,
						&truncate_to);
#else
				smgrtruncate(
						RelationGetSmgr(index), &forknum, 1, &truncate_to);
#endif
			}
		}
	}

	/* Update metapage with L0 chain and statistics */
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;

		metabuf = ReadBuffer(index, 0);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		metap->level_heads[0]  = shared->segment_head;
		metap->level_counts[0] = shared->segment_count;
		metap->total_docs = (int32)pg_atomic_read_u64(&shared->total_docs);
		metap->total_len  = (int64)pg_atomic_read_u64(&shared->total_len);

		MarkBufferDirty(metabuf);
		UnlockReleaseBuffer(metabuf);

		/* Run compaction if needed */
		tp_maybe_compact_level(index, 0);
	}

	/* Build result */
	result				 = palloc0(sizeof(IndexBuildResult));
	result->heap_tuples	 = (double)pg_atomic_read_u64(&shared->total_docs);
	result->index_tuples = (double)pg_atomic_read_u64(&shared->total_docs);

	/* Cleanup */
	dsa_detach(dsa);
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
		int					   nworkers,
		dsa_area			  *dsa)
{
	TpWorkerState *worker_states;
	int			   i;
	Size		   memory_budget;

	memset(shared, 0, sizeof(TpParallelBuildShared));

	/* Immutable configuration */
	shared->heaprelid		= RelationGetRelid(heap);
	shared->indexrelid		= RelationGetRelid(index);
	shared->text_config_oid = text_config_oid;
	shared->attnum			= attnum;
	shared->k1				= k1;
	shared->b				= b;
	shared->nworkers		= nworkers;

	/* DSA handle for workers to attach */
	shared->memtable_dsa_handle = dsa_get_handle(dsa);

	/* Per-worker memory budget (split maintenance_work_mem across workers) */
	memory_budget = ((Size)maintenance_work_mem * 1024) / nworkers;
	shared->memory_budget_per_worker = (Size)(memory_budget *
											  TP_MEMORY_SLOP_FACTOR);

	/* Initialize coordination primitives */
	SpinLockInit(&shared->mutex);
	ConditionVariableInit(&shared->leader_wake_cv);
	shared->workers_done = 0;

	/* Initialize atomic counters */
	pg_atomic_init_u32(&shared->pool_next, 0);
	pg_atomic_init_u64(&shared->total_docs, 0);
	pg_atomic_init_u64(&shared->total_len, 0);

	/* Initialize segment tracking */
	shared->segment_head  = InvalidBlockNumber;
	shared->segment_tail  = InvalidBlockNumber;
	shared->segment_count = 0;

	/* Initialize per-worker state */
	worker_states = TpParallelWorkerStates(shared);
	for (i = 0; i < nworkers; i++)
	{
		int j;

		pg_atomic_init_u32(&worker_states[i].active_buffer, 0);
		pg_atomic_init_u32(&worker_states[i].scan_complete, 0);
		pg_atomic_init_u64(&worker_states[i].tuples_scanned, 0);

		ConditionVariableInit(&worker_states[i].buffer_ready_cv);
		ConditionVariableInit(&worker_states[i].buffer_consumed_cv);

		/* Initialize both buffers - leader creates all hash tables upfront */
		for (j = 0; j < 2; j++)
		{
			dshash_table *string_table;
			dshash_table *doclength_table;

			pg_atomic_init_u32(
					&worker_states[i].buffers[j].status, TP_BUFFER_EMPTY);

			/* Create string hash table */
			string_table = tp_worker_create_string_table(dsa);
			worker_states[i].buffers[j].string_hash_handle =
					dshash_get_hash_table_handle(string_table);
			dshash_detach(string_table);

			/* Create document length hash table */
			doclength_table = tp_worker_create_doclength_table(dsa);
			worker_states[i].buffers[j].doc_lengths_handle =
					dshash_get_hash_table_handle(doclength_table);
			dshash_detach(doclength_table);

			worker_states[i].buffers[j].num_docs	= 0;
			worker_states[i].buffers[j].num_terms	= 0;
			worker_states[i].buffers[j].total_len	= 0;
			worker_states[i].buffers[j].memory_used = 0;
		}
	}
}

/*
 * Pre-allocate page pool for segment writes
 */
static void
tp_preallocate_page_pool(Relation index, TpParallelBuildShared *shared)
{
	BlockNumber *pool;
	int			 i;

	pool = TpParallelPagePool(shared);

	/* Extend relation and collect block numbers */
	for (i = 0; i < shared->total_pool_pages; i++)
	{
		Buffer buf = ReadBufferExtended(
				index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
		pool[i] = BufferGetBlockNumber(buf);
		PageInit(BufferGetPage(buf), BLCKSZ, 0);
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
	}

	/* Flush to ensure durability */
	smgrimmedsync(RelationGetSmgr(index), MAIN_FORKNUM);

	elog(DEBUG1,
		 "Pre-allocated %d pages for parallel build (blocks %u-%u)",
		 shared->total_pool_pages,
		 pool[0],
		 pool[shared->total_pool_pages - 1]);
}

/*
 * Get a page from the pool (called by leader during segment write)
 */
BlockNumber
tp_pool_get_page(TpParallelBuildShared *shared)
{
	BlockNumber *pool = TpParallelPagePool(shared);
	uint32		 idx  = pg_atomic_fetch_add_u32(&shared->pool_next, 1);

	if (idx >= (uint32)shared->total_pool_pages)
	{
		elog(ERROR,
			 "Parallel build page pool exhausted (%d pages). "
			 "Increase pg_textsearch.parallel_build_expansion_factor.",
			 shared->total_pool_pages);
	}

	return pool[idx];
}

/*
 * Worker entry point - called by parallel infrastructure
 */
PGDLLEXPORT void
tp_parallel_build_worker_main(
		dsm_segment *seg __attribute__((unused)), shm_toc *toc)
{
	TpParallelBuildShared *shared;
	TpWorkerState		  *my_state;
	Relation			   heap;
	TableScanDesc		   scan;
	TupleTableSlot		  *slot;
	dsa_area			  *dsa;
	int					   worker_id;
	int					   active_buf;

	/* Attach to shared memory */
	shared = (TpParallelBuildShared *)
			shm_toc_lookup(toc, TP_PARALLEL_KEY_SHARED, false);

	worker_id = ParallelWorkerNumber;
	my_state  = &TpParallelWorkerStates(shared)[worker_id];

	/* Attach to shared DSA */
	dsa = dsa_attach(shared->memtable_dsa_handle);
	dsa_pin_mapping(dsa);

	/* Open heap relation */
	heap = table_open(shared->heaprelid, AccessShareLock);

	/* Join parallel table scan */
	scan = table_beginscan_parallel(heap, TpParallelTableScan(shared));
	slot = table_slot_create(heap, NULL);

	/* Start with buffer 0 */
	active_buf = 0;
	pg_atomic_write_u32(&my_state->active_buffer, active_buf);
	pg_atomic_write_u32(
			&my_state->buffers[active_buf].status, TP_BUFFER_FILLING);

	/* Process tuples */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		TpWorkerMemtableBuffer *buffer = &my_state->buffers[active_buf];

		/* Check if we need to switch buffers */
		if (buffer->memory_used >= shared->memory_budget_per_worker)
		{
			/* Mark current buffer as ready */
			pg_atomic_write_u32(&buffer->status, TP_BUFFER_READY);
			ConditionVariableSignal(&my_state->buffer_ready_cv);
			ConditionVariableSignal(&shared->leader_wake_cv);

			/* Switch to other buffer */
			active_buf = 1 - active_buf;
			buffer	   = &my_state->buffers[active_buf];

			/* Wait for other buffer to be empty */
			while (pg_atomic_read_u32(&buffer->status) != TP_BUFFER_EMPTY)
			{
				ConditionVariableSleep(
						&my_state->buffer_consumed_cv,
						WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
			}
			ConditionVariableCancelSleep();

			/* Start filling the new buffer */
			pg_atomic_write_u32(&my_state->active_buffer, active_buf);
			pg_atomic_write_u32(&buffer->status, TP_BUFFER_FILLING);
		}

		/* Process document into current buffer */
		tp_worker_process_document(
				dsa,
				&my_state->buffers[active_buf],
				slot,
				shared->attnum,
				shared->text_config_oid);

		pg_atomic_fetch_add_u64(&my_state->tuples_scanned, 1);

		CHECK_FOR_INTERRUPTS();
	}

	/* Mark final buffer as ready if it has data */
	{
		TpWorkerMemtableBuffer *buffer = &my_state->buffers[active_buf];
		if (buffer->num_docs > 0)
		{
			pg_atomic_write_u32(&buffer->status, TP_BUFFER_READY);
			ConditionVariableSignal(&my_state->buffer_ready_cv);
			ConditionVariableSignal(&shared->leader_wake_cv);
		}
		else
		{
			pg_atomic_write_u32(&buffer->status, TP_BUFFER_EMPTY);
		}
	}

	/* Mark scan complete */
	pg_atomic_write_u32(&my_state->scan_complete, 1);

	/*
	 * Note: Global stats (total_docs, total_len) are accumulated by the leader
	 * when processing each buffer, not by workers. This is because the leader
	 * clears buffer stats after processing, so worker accumulation would be
	 * incorrect.
	 */

	/* Signal completion */
	SpinLockAcquire(&shared->mutex);
	shared->workers_done++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->leader_wake_cv);

	/* Cleanup */
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	table_close(heap, AccessShareLock);
	dsa_detach(dsa);
}

/*
 * Leader processes worker buffers and writes segments
 */
static void
tp_leader_process_buffers(
		TpParallelBuildShared *shared, Relation index, dsa_area *dsa)
{
	TpWorkerState *worker_states = TpParallelWorkerStates(shared);
	bool		   all_done		 = false;

	while (!all_done)
	{
		int	 i;
		bool found_work = false;

		/* Check each worker for ready buffers */
		for (i = 0; i < shared->nworkers; i++)
		{
			int j;
			for (j = 0; j < 2; j++)
			{
				TpWorkerMemtableBuffer *buffer = &worker_states[i].buffers[j];
				uint32 status = pg_atomic_read_u32(&buffer->status);

				if (status == TP_BUFFER_READY)
				{
					BlockNumber seg_block;

					found_work = true;

					/* Write segment from this buffer */
					seg_block = tp_write_segment_from_worker_buffer(
							shared, buffer, index, dsa);

					if (seg_block != InvalidBlockNumber)
					{
						/* Chain into L0 */
						if (shared->segment_head == InvalidBlockNumber)
						{
							shared->segment_head = seg_block;
							shared->segment_tail = seg_block;
						}
						else
						{
							/* Link previous tail to new segment */
							Buffer			 tail_buf;
							Page			 tail_page;
							TpSegmentHeader *tail_header;

							tail_buf = ReadBuffer(index, shared->segment_tail);
							LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
							tail_page = BufferGetPage(tail_buf);
							tail_header =
									(TpSegmentHeader *)((char *)tail_page +
														SizeOfPageHeaderData);
							tail_header->next_segment = seg_block;
							MarkBufferDirty(tail_buf);
							UnlockReleaseBuffer(tail_buf);

							shared->segment_tail = seg_block;
						}
						shared->segment_count++;
					}

					/* Accumulate stats before clearing buffer */
					pg_atomic_fetch_add_u64(
							&shared->total_docs, buffer->num_docs);
					pg_atomic_fetch_add_u64(
							&shared->total_len, buffer->total_len);

					/* Clear buffer's hash tables for reuse */
					if (buffer->string_hash_handle != 0)
					{
						dshash_table	 *old_string_table;
						dshash_table	 *new_string_table;
						dshash_parameters string_params;

						/* Detach and destroy old string table */
						string_params.key_size	 = sizeof(TpStringKey);
						string_params.entry_size = sizeof(TpStringHashEntry);
						string_params.hash_function = tp_worker_string_hash;
						string_params.compare_function =
								tp_worker_string_compare;
						string_params.copy_function = dshash_memcpy;
						string_params.tranche_id = TP_STRING_HASH_TRANCHE_ID;
						old_string_table		 = dshash_attach(
								dsa,
								&string_params,
								buffer->string_hash_handle,
								dsa);
						dshash_destroy(old_string_table);

						/* Create fresh string table */
						new_string_table = tp_worker_create_string_table(dsa);
						buffer->string_hash_handle =
								dshash_get_hash_table_handle(new_string_table);
						dshash_detach(new_string_table);
					}

					if (buffer->doc_lengths_handle != 0)
					{
						dshash_table	 *old_doc_table;
						dshash_table	 *new_doc_table;
						dshash_parameters doc_params;

						/* Detach and destroy old doc lengths table */
						doc_params.key_size			= sizeof(ItemPointerData);
						doc_params.entry_size		= sizeof(TpDocLengthEntry);
						doc_params.hash_function	= dshash_memhash;
						doc_params.compare_function = dshash_memcmp;
						doc_params.copy_function	= dshash_memcpy;
						doc_params.tranche_id = TP_DOCLENGTH_HASH_TRANCHE_ID;
						old_doc_table		  = dshash_attach(
								dsa,
								&doc_params,
								buffer->doc_lengths_handle,
								dsa);
						dshash_destroy(old_doc_table);

						/* Create fresh doc lengths table */
						new_doc_table = tp_worker_create_doclength_table(dsa);
						buffer->doc_lengths_handle =
								dshash_get_hash_table_handle(new_doc_table);
						dshash_detach(new_doc_table);
					}

					/* Clear buffer stats and signal worker */
					buffer->num_docs	= 0;
					buffer->num_terms	= 0;
					buffer->total_len	= 0;
					buffer->memory_used = 0;

					pg_atomic_write_u32(&buffer->status, TP_BUFFER_EMPTY);
					ConditionVariableSignal(
							&worker_states[i].buffer_consumed_cv);
				}
			}
		}

		/* Check if all workers are done */
		SpinLockAcquire(&shared->mutex);
		all_done = (shared->workers_done >= shared->nworkers);
		SpinLockRelease(&shared->mutex);

		/* If no work found and not done, wait */
		if (!found_work && !all_done)
		{
			ConditionVariableSleep(
					&shared->leader_wake_cv,
					WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
			ConditionVariableCancelSleep();
		}

		CHECK_FOR_INTERRUPTS();
	}

	/* Process any remaining ready buffers */
	for (int i = 0; i < shared->nworkers; i++)
	{
		for (int j = 0; j < 2; j++)
		{
			TpWorkerMemtableBuffer *buffer = &worker_states[i].buffers[j];
			if (pg_atomic_read_u32(&buffer->status) == TP_BUFFER_READY)
			{
				/* Accumulate stats before processing */
				pg_atomic_fetch_add_u64(&shared->total_docs, buffer->num_docs);
				pg_atomic_fetch_add_u64(&shared->total_len, buffer->total_len);

				BlockNumber seg_block = tp_write_segment_from_worker_buffer(
						shared, buffer, index, dsa);
				if (seg_block != InvalidBlockNumber)
				{
					if (shared->segment_head == InvalidBlockNumber)
					{
						shared->segment_head = seg_block;
						shared->segment_tail = seg_block;
					}
					else
					{
						Buffer			 tail_buf;
						Page			 tail_page;
						TpSegmentHeader *tail_header;

						tail_buf = ReadBuffer(index, shared->segment_tail);
						LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
						tail_page = BufferGetPage(tail_buf);
						tail_header =
								(TpSegmentHeader *)((char *)tail_page +
													SizeOfPageHeaderData);
						tail_header->next_segment = seg_block;
						MarkBufferDirty(tail_buf);
						UnlockReleaseBuffer(tail_buf);

						shared->segment_tail = seg_block;
					}
					shared->segment_count++;
				}
			}
		}
	}
}

/*
 * dshash parameters for string table (parallel-safe version)
 * Uses LWTRANCHE_PARALLEL_HASH_JOIN which is registered in parallel workers.
 */
/*
 * Get string and length from key.
 * Lookup keys use explicit len field; stored keys use strlen on DSA string.
 */
static inline void
tp_worker_get_key_str_len(
		const TpStringKey *key,
		dsa_area		  *dsa,
		const char		 **str_out,
		size_t			  *len_out)
{
	if (DsaPointerIsValid(key->posting_list))
	{
		/* Stored key - string is null-terminated in DSA */
		*str_out = dsa_get_address(dsa, key->term.dp);
		*len_out = strlen(*str_out);
	}
	else
	{
		/* Lookup key - use explicit length (no null termination required) */
		*str_out = key->term.str;
		*len_out = key->len;
	}
}

static uint32
tp_worker_string_hash(const void *key, size_t keysize, void *arg)
{
	const TpStringKey *skey = (const TpStringKey *)key;
	dsa_area		  *dsa	= (dsa_area *)arg;
	const char		  *str;
	size_t			   len;

	Assert(keysize == sizeof(TpStringKey));

	tp_worker_get_key_str_len(skey, dsa, &str, &len);

	return hash_bytes((const unsigned char *)str, len);
}

static int
tp_worker_string_compare(
		const void *a, const void *b, size_t keysize, void *arg)
{
	const TpStringKey *key_a = (const TpStringKey *)a;
	const TpStringKey *key_b = (const TpStringKey *)b;
	dsa_area		  *dsa	 = (dsa_area *)arg;
	const char		  *str_a;
	const char		  *str_b;
	size_t			   len_a;
	size_t			   len_b;

	Assert(keysize == sizeof(TpStringKey));

	tp_worker_get_key_str_len(key_a, dsa, &str_a, &len_a);
	tp_worker_get_key_str_len(key_b, dsa, &str_b, &len_b);

	/* Compare by length first for efficiency */
	if (len_a != len_b)
		return (len_a < len_b) ? -1 : 1;

	/* Same length - compare contents */
	return memcmp(str_a, str_b, len_a);
}

/*
 * Create a new string hash table for worker buffer (parallel-safe)
 */
static dshash_table *
tp_worker_create_string_table(dsa_area *dsa)
{
	dshash_parameters params;

	params.key_size			= sizeof(TpStringKey);
	params.entry_size		= sizeof(TpStringHashEntry);
	params.hash_function	= tp_worker_string_hash;
	params.compare_function = tp_worker_string_compare;
	params.copy_function	= dshash_memcpy;
	params.tranche_id		= TP_STRING_HASH_TRANCHE_ID;

	return dshash_create(dsa, &params, dsa);
}

/*
 * Create a new document length hash table for worker buffer (parallel-safe)
 */
static dshash_table *
tp_worker_create_doclength_table(dsa_area *dsa)
{
	dshash_parameters params;

	params.key_size			= sizeof(ItemPointerData);
	params.entry_size		= sizeof(TpDocLengthEntry);
	params.hash_function	= dshash_memhash;
	params.compare_function = dshash_memcmp;
	params.copy_function	= dshash_memcpy;
	params.tranche_id		= TP_DOCLENGTH_HASH_TRANCHE_ID;

	return dshash_create(dsa, &params, dsa);
}

/*
 * Process a single document into worker's memtable buffer
 *
 * Creates hash tables on first document, then adds all terms to the
 * buffer's string hash table and stores the document length.
 */
static void
tp_worker_process_document(
		dsa_area			   *dsa,
		TpWorkerMemtableBuffer *buffer,
		TupleTableSlot		   *slot,
		int						attnum,
		Oid						text_config_oid)
{
	bool		  isnull;
	Datum		  text_datum;
	text		 *document_text;
	ItemPointer	  ctid;
	Datum		  tsvector_datum;
	TSVector	  tsvector;
	int32		  doc_length = 0;
	WordEntry	 *we;
	int			  i;
	dshash_table *string_table;
	dshash_table *doclength_table;
	Size		  memory_delta = 0;

	/* Get text value */
	text_datum = slot_getattr(slot, attnum, &isnull);
	if (isnull)
		return;

	document_text = DatumGetTextP(text_datum);
	ctid		  = &slot->tts_tid;

	if (!ItemPointerIsValid(ctid))
		return;

	/* Tokenize document */
	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid,
			ObjectIdGetDatum(text_config_oid),
			PointerGetDatum(document_text));
	tsvector = DatumGetTSVector(tsvector_datum);

	if (tsvector->size == 0)
		return;

	we = ARRPTR(tsvector);

	/* Count document length first */
	for (i = 0; i < tsvector->size; i++)
	{
		int32 frequency;
		if (we[i].haspos)
			frequency = (int32)POSDATALEN(tsvector, &we[i]);
		else
			frequency = 1;
		doc_length += frequency;
	}

	/*
	 * Attach to hash tables for this buffer.
	 * Hash tables are created by the leader during initialization.
	 */
	{
		dshash_parameters string_params;
		string_params.key_size		   = sizeof(TpStringKey);
		string_params.entry_size	   = sizeof(TpStringHashEntry);
		string_params.hash_function	   = tp_worker_string_hash;
		string_params.compare_function = tp_worker_string_compare;
		string_params.copy_function	   = dshash_memcpy;
		string_params.tranche_id	   = TP_STRING_HASH_TRANCHE_ID;
		string_table				   = dshash_attach(
				  dsa, &string_params, buffer->string_hash_handle, dsa);
	}

	{
		dshash_parameters doc_params;
		doc_params.key_size			= sizeof(ItemPointerData);
		doc_params.entry_size		= sizeof(TpDocLengthEntry);
		doc_params.hash_function	= dshash_memhash;
		doc_params.compare_function = dshash_memcmp;
		doc_params.copy_function	= dshash_memcpy;
		doc_params.tranche_id		= TP_DOCLENGTH_HASH_TRANCHE_ID;
		doclength_table				= dshash_attach(
				dsa, &doc_params, buffer->doc_lengths_handle, dsa);
	}

	/* Add each term to the string hash table */
	for (i = 0; i < tsvector->size; i++)
	{
		char			  *term_text;
		size_t			   term_len;
		int32			   frequency;
		TpStringHashEntry *entry;
		TpPostingList	  *posting_list;
		TpPostingEntry	  *entries;
		TpStringKey		   lookup_key;
		bool			   found;

		/* Get term text (NOT null-terminated in TSVector) */
		term_text = STRPTR(tsvector) + we[i].pos;
		term_len  = we[i].len;

		/* Get frequency */
		if (we[i].haspos)
			frequency = (int32)POSDATALEN(tsvector, &we[i]);
		else
			frequency = 1;

		/* Build lookup key with explicit length (no allocation needed) */
		lookup_key.term.str		= term_text;
		lookup_key.posting_list = InvalidDsaPointer; /* Indicates lookup key */
		lookup_key.len			= term_len;

		/* Find or insert term using dshash directly */
		entry = (TpStringHashEntry *)
				dshash_find_or_insert(string_table, &lookup_key, &found);

		/* If new entry, allocate DSA string and posting list */
		if (!found)
		{
			dsa_pointer string_dp;
			char	   *stored_string;

			/* Allocate string in DSA */
			string_dp	  = dsa_allocate(dsa, term_len + 1);
			stored_string = dsa_get_address(dsa, string_dp);
			memcpy(stored_string, term_text, term_len);
			stored_string[term_len] = '\0';

			/* Convert key from lookup (char*) to storage (dsa_pointer) */
			entry->key.term.dp		= string_dp;
			entry->key.posting_list = tp_alloc_posting_list(dsa);

			buffer->num_terms++;
			memory_delta += term_len + 1 + sizeof(TpPostingList) + 64;
		}

		posting_list = dsa_get_address(dsa, entry->key.posting_list);

		/* Grow posting list if needed */
		if (posting_list->doc_count >= posting_list->capacity)
		{
			int32		new_capacity;
			Size		new_size;
			dsa_pointer new_entries_dp;

			new_capacity = posting_list->capacity == 0
								 ? 8
								 : posting_list->capacity * 2;
			new_size	 = new_capacity * sizeof(TpPostingEntry);

			new_entries_dp = dsa_allocate(dsa, new_size);
			if (!DsaPointerIsValid(new_entries_dp))
				elog(ERROR, "Failed to allocate posting entries in worker");

			/* Copy existing entries if any */
			if (posting_list->doc_count > 0 &&
				DsaPointerIsValid(posting_list->entries_dp))
			{
				TpPostingEntry *old_entries =
						dsa_get_address(dsa, posting_list->entries_dp);
				TpPostingEntry *new_entries =
						dsa_get_address(dsa, new_entries_dp);
				memcpy(new_entries,
					   old_entries,
					   posting_list->doc_count * sizeof(TpPostingEntry));
				dsa_free(dsa, posting_list->entries_dp);
			}

			posting_list->entries_dp = new_entries_dp;
			posting_list->capacity	 = new_capacity;
			memory_delta += new_size;
		}

		/* Add document to posting list */
		entries = dsa_get_address(dsa, posting_list->entries_dp);
		entries[posting_list->doc_count].ctid	   = *ctid;
		entries[posting_list->doc_count].frequency = frequency;
		posting_list->doc_count++;
		posting_list->doc_freq = posting_list->doc_count;

		/* Release lock on string entry */
		dshash_release_lock(string_table, entry);
	}

	/* Store document length */
	{
		TpDocLengthEntry *doc_entry;
		bool			  found;

		doc_entry		= dshash_find_or_insert(doclength_table, ctid, &found);
		doc_entry->ctid = *ctid;
		doc_entry->doc_length = doc_length;
		dshash_release_lock(doclength_table, doc_entry);

		if (!found)
			memory_delta += sizeof(TpDocLengthEntry);
	}

	/* Detach from hash tables */
	dshash_detach(string_table);
	dshash_detach(doclength_table);

	/* Update statistics */
	buffer->num_docs++;
	buffer->total_len += doc_length;
	buffer->memory_used += memory_delta;
}

/*
 * Per-term block information for segment writing (copied from segment.c)
 */
typedef struct WorkerTermBlockInfo
{
	uint32 posting_offset;	 /* Absolute offset where postings were written */
	uint16 block_count;		 /* Number of blocks for this term */
	uint32 doc_freq;		 /* Document frequency */
	uint32 skip_entry_start; /* Index into accumulated skip entries array */
} WorkerTermBlockInfo;

/*
 * Comparison function for sorting terms
 */
static int
worker_term_info_cmp(const void *a, const void *b)
{
	const TermInfo *ta = (const TermInfo *)a;
	const TermInfo *tb = (const TermInfo *)b;
	return strcmp(ta->term, tb->term);
}

/*
 * Build docmap from worker buffer's doc_lengths hash table
 */
static TpDocMapBuilder *
build_docmap_from_worker_buffer(dsa_area *dsa, TpWorkerMemtableBuffer *buffer)
{
	TpDocMapBuilder	 *docmap;
	dshash_table	 *doclength_table;
	dshash_seq_status seq_status;
	TpDocLengthEntry *doc_entry;
	dshash_parameters params;

	docmap = tp_docmap_create();

	if (buffer->doc_lengths_handle == 0)
		return docmap;

	/* Setup parameters for doc lengths hash table (parallel-safe) */
	params.key_size			= sizeof(ItemPointerData);
	params.entry_size		= sizeof(TpDocLengthEntry);
	params.hash_function	= dshash_memhash;
	params.compare_function = dshash_memcmp;
	params.copy_function	= dshash_memcpy;
	params.tranche_id		= TP_DOCLENGTH_HASH_TRANCHE_ID;

	/* Attach to document lengths hash table */
	doclength_table =
			dshash_attach(dsa, &params, buffer->doc_lengths_handle, dsa);

	/* Iterate through all documents and add to docmap */
	dshash_seq_init(&seq_status, doclength_table, false);
	while ((doc_entry = (TpDocLengthEntry *)dshash_seq_next(&seq_status)) !=
		   NULL)
	{
		tp_docmap_add(docmap, &doc_entry->ctid, (uint32)doc_entry->doc_length);
	}
	dshash_seq_term(&seq_status);
	dshash_detach(doclength_table);

	/* Finalize to build output arrays */
	tp_docmap_finalize(docmap);

	return docmap;
}

/*
 * Build sorted dictionary from worker buffer's string hash table
 */
static TermInfo *
build_dictionary_from_worker_buffer(
		dsa_area *dsa, TpWorkerMemtableBuffer *buffer, uint32 *num_terms)
{
	dshash_table	  *string_table;
	dshash_seq_status  status;
	TpStringHashEntry *entry;
	TermInfo		  *terms;
	uint32			   count	= 0;
	uint32			   capacity = 1024;
	dshash_parameters  params;

	if (buffer->string_hash_handle == 0)
	{
		*num_terms = 0;
		return NULL;
	}

	/* Attach to string hash table using parallel-safe parameters */
	params.key_size			= sizeof(TpStringKey);
	params.entry_size		= sizeof(TpStringHashEntry);
	params.hash_function	= tp_worker_string_hash;
	params.compare_function = tp_worker_string_compare;
	params.copy_function	= dshash_memcpy;
	params.tranche_id		= TP_STRING_HASH_TRANCHE_ID;
	string_table =
			dshash_attach(dsa, &params, buffer->string_hash_handle, dsa);

	/* Allocate initial array */
	terms = palloc(sizeof(TermInfo) * capacity);

	/* Iterate through all terms in string table */
	dshash_seq_init(&status, string_table, false);

	while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) != NULL)
	{
		const char *term = tp_get_key_str(dsa, &entry->key);

		if (!term)
			continue;

		/* Resize if needed */
		if (count >= capacity)
		{
			capacity *= 2;
			terms = repalloc(terms, sizeof(TermInfo) * capacity);
		}

		/* Store term info */
		terms[count].term			 = pstrdup(term);
		terms[count].term_len		 = strlen(term);
		terms[count].posting_list_dp = entry->key.posting_list;
		terms[count].dict_entry_idx	 = count;
		count++;
	}

	dshash_seq_term(&status);
	dshash_detach(string_table);

	/* Sort terms lexicographically */
	qsort(terms, count, sizeof(TermInfo), worker_term_info_cmp);

	*num_terms = count;
	return terms;
}

/*
 * Write segment from worker's memtable buffer
 *
 * Reads terms and postings from the buffer's dshash tables and writes
 * a segment using the page pool for contiguous allocation.
 */
static BlockNumber
tp_write_segment_from_worker_buffer(
		TpParallelBuildShared  *shared,
		TpWorkerMemtableBuffer *buffer,
		Relation				index,
		dsa_area			   *dsa)
{
	TermInfo			*terms;
	uint32				 num_terms;
	BlockNumber			 header_block;
	BlockNumber			 page_index_root;
	TpSegmentWriter		 writer;
	TpSegmentHeader		 header;
	TpDictionary		 dict;
	TpDocMapBuilder		*docmap;
	BlockNumber			*page_pool;
	pg_atomic_uint32	*pool_next;
	uint32				*string_offsets;
	uint32				 string_pos;
	uint32				 i;
	Buffer				 header_buf;
	Page				 header_page;
	TpSegmentHeader		*existing_header;
	WorkerTermBlockInfo *term_blocks;
	TpSkipEntry			*all_skip_entries;
	uint32				 skip_entries_count;
	uint32				 skip_entries_capacity;

	/* Skip if buffer has no data */
	if (buffer->num_docs == 0)
		return InvalidBlockNumber;

	/* Build docmap from buffer */
	docmap = build_docmap_from_worker_buffer(dsa, buffer);

	/* Build sorted dictionary from buffer */
	terms = build_dictionary_from_worker_buffer(dsa, buffer, &num_terms);

	if (num_terms == 0)
	{
		tp_docmap_destroy(docmap);
		return InvalidBlockNumber;
	}

	/* Get page pool from shared state */
	page_pool = TpParallelPagePool(shared);
	pool_next = &shared->pool_next;

	/* Initialize writer with page pool */
	tp_segment_writer_init_with_pool(
			&writer, index, page_pool, shared->total_pool_pages, pool_next);

	if (writer.pages_allocated > 0)
	{
		header_block = writer.pages[0];
	}
	else
	{
		elog(ERROR,
			 "tp_write_segment_from_worker_buffer: "
			 "Failed to allocate first page");
	}

	/* Initialize header */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_VERSION;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= 0;
	header.num_terms	= num_terms;
	header.level		= 0;
	header.next_segment = InvalidBlockNumber;

	/* Dictionary immediately follows header */
	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Get corpus statistics from buffer */
	header.num_docs		= buffer->num_docs;
	header.total_tokens = buffer->total_len;

	/* Write placeholder header */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary section */
	dict.num_terms = num_terms;
	tp_segment_writer_write(
			&writer, &dict, offsetof(TpDictionary, string_offsets));

	/* Build string offsets */
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

	/* Postings start here */
	header.postings_offset = writer.current_offset;

	/* Initialize per-term tracking and skip entry accumulator */
	term_blocks = palloc0(num_terms * sizeof(WorkerTermBlockInfo));

	skip_entries_capacity = 1024;
	skip_entries_count	  = 0;
	all_skip_entries = palloc(skip_entries_capacity * sizeof(TpSkipEntry));

	/*
	 * Streaming pass: for each term, convert postings and write immediately.
	 */
	for (i = 0; i < num_terms; i++)
	{
		TpPostingList  *posting_list = NULL;
		TpPostingEntry *entries		 = NULL;
		uint32			doc_count	 = 0;
		uint32			block_idx;
		uint32			num_blocks;
		TpBlockPosting *block_postings = NULL;

		/* Record where this term's postings start */
		term_blocks[i].posting_offset	= writer.current_offset;
		term_blocks[i].skip_entry_start = skip_entries_count;

		if (DsaPointerIsValid(terms[i].posting_list_dp))
		{
			posting_list = (TpPostingList *)
					dsa_get_address(dsa, terms[i].posting_list_dp);
			if (posting_list && posting_list->doc_count > 0)
			{
				entries = (TpPostingEntry *)
						dsa_get_address(dsa, posting_list->entries_dp);
				doc_count = posting_list->doc_count;
			}
		}

		term_blocks[i].doc_freq = posting_list ? posting_list->doc_freq : 0;

		if (doc_count == 0)
		{
			term_blocks[i].block_count = 0;
			continue;
		}

		/* Calculate number of blocks */
		num_blocks = (doc_count + TP_BLOCK_SIZE - 1) / TP_BLOCK_SIZE;
		term_blocks[i].block_count = (uint16)num_blocks;

		/* Convert postings to block format */
		block_postings = palloc(doc_count * sizeof(TpBlockPosting));
		{
			uint32 j;
			for (j = 0; j < doc_count; j++)
			{
				uint32 doc_id = tp_docmap_lookup(docmap, &entries[j].ctid);
				uint8  norm;

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
		}

		/* Write posting blocks and build skip entries */
		for (block_idx = 0; block_idx < num_blocks; block_idx++)
		{
			TpSkipEntry skip;
			uint32		block_start = block_idx * TP_BLOCK_SIZE;
			uint32 block_end = Min(block_start + TP_BLOCK_SIZE, doc_count);
			uint32 j;
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

			/* Build skip entry */
			skip.last_doc_id	= last_doc_id;
			skip.doc_count		= (uint8)(block_end - block_start);
			skip.block_max_tf	= max_tf;
			skip.block_max_norm = max_norm;
			skip.posting_offset = writer.current_offset;
			skip.flags			= TP_BLOCK_FLAG_UNCOMPRESSED;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			/* Write posting block data (uncompressed for simplicity) */
			tp_segment_writer_write(
					&writer,
					&block_postings[block_start],
					(block_end - block_start) * sizeof(TpBlockPosting));

			/* Accumulate skip entry */
			if (skip_entries_count >= skip_entries_capacity)
			{
				skip_entries_capacity *= 2;
				all_skip_entries = repalloc(
						all_skip_entries,
						skip_entries_capacity * sizeof(TpSkipEntry));
			}
			all_skip_entries[skip_entries_count++] = skip;
		}

		pfree(block_postings);
	}

	/* Skip index starts here */
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

	/* Update num_docs to actual count from docmap */
	header.num_docs = docmap->num_docs;

	/* Flush writer and write page index */
	tp_segment_writer_flush(&writer);

	/* Prevent double flush in tp_segment_writer_finish */
	writer.buffer_pos = SizeOfPageHeaderData;

	page_index_root = write_page_index_from_pool(
			index,
			writer.pages,
			writer.pages_allocated,
			page_pool,
			shared->total_pool_pages,
			pool_next);
	header.page_index = page_index_root;

	/* Update header with actual values */
	header.data_size = writer.current_offset;
	header.num_pages = writer.pages_allocated;

	/*
	 * Write dictionary entries with correct skip_index_offset values.
	 */
	{
		Buffer dict_buf;
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
						 errmsg("dict entry %u logical page %u >= "
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

			/* Write entry to page */
			{
				uint32 bytes_on_this_page = SEGMENT_DATA_PER_PAGE -
											page_offset;

				if (bytes_on_this_page >= sizeof(TpDictEntry))
				{
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

					memcpy(dest, src, bytes_on_this_page);

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

	/* Update header on disk */
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page = BufferGetPage(header_buf);

	existing_header					= (TpSegmentHeader *)((char *)header_page +
										  SizeOfPageHeaderData);
	existing_header->strings_offset = header.strings_offset;
	existing_header->entries_offset = header.entries_offset;
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
	pfree(term_blocks);
	pfree(string_offsets);
	tp_free_dictionary(terms, num_terms);
	tp_docmap_destroy(docmap);

	return header_block;
}
