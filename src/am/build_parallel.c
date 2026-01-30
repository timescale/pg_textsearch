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
 * - All disk I/O is done by the leader using P_NEW for dynamic allocation
 *
 * This design maximizes parallelism by overlapping heap scanning (workers)
 * with segment writing (leader).
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
#include "segment/compression.h"
#include "segment/dictionary.h"
#include "segment/docmap.h"
#include "segment/merge.h"
#include "segment/pagemapper.h"
#include "segment/segment.h"
#include "state/metapage.h"

/* GUC: compression for segments */
extern bool tp_compress_segments;

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

static void tp_leader_process_buffers(
		TpParallelBuildShared *shared, Relation index, dsa_area *dsa);

static BlockNumber tp_merge_worker_buffers_to_segment(
		TpParallelBuildShared	*shared,
		TpWorkerMemtableBuffer **buffers,
		int						 num_buffers,
		Relation				 index,
		dsa_area				*dsa);

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
static TermInfo *build_dictionary_from_worker_buffer(
		dsa_area *dsa, TpWorkerMemtableBuffer *buffer, uint32 *num_terms);

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

	/* Per-worker state array */
	size = add_size(size, MAXALIGN(sizeof(TpWorkerState) * nworkers));

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
	dsa_area			  *dsa;
	int					   launched;

	/* Ensure reasonable number of workers */
	if (nworkers > TP_MAX_PARALLEL_WORKERS)
		nworkers = TP_MAX_PARALLEL_WORKERS;

	/* Get snapshot for parallel scan */
	snapshot = GetTransactionSnapshot();
	snapshot = RegisterSnapshot(snapshot);

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

	/* Initialize parallel table scan */
	table_parallelscan_initialize(heap, TpParallelTableScan(shared), snapshot);

	/* Insert shared state into TOC */
	shm_toc_insert(pcxt->toc, TP_PARALLEL_KEY_SHARED, shared);

	/* Launch workers */
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;

	ereport(NOTICE,
			(errmsg("parallel index build: launched %d of %d requested workers",
					launched, nworkers)));

	/*
	 * Leader processes worker memtables and writes segments.
	 * This continues until all workers are done.
	 */
	tp_leader_process_buffers(shared, index, dsa);

	/* Wait for all workers to finish */
	WaitForParallelWorkersToFinish(pcxt);

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

		/* Run compaction if needed (threshold is 8 segments per level) */
		tp_maybe_compact_level(index, 0);
	}

	/*
	 * Final truncation: after compaction, L0 segment pages are freed but the
	 * file isn't shrunk. We truncate to the minimum needed size by finding
	 * the highest used block from segment locations.
	 *
	 * We use tp_segment_collect_pages to get ALL pages (data + page index)
	 * since page index pages can be located anywhere in the file.
	 */
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;
		BlockNumber		max_used = 1; /* At least metapage */
		int				level;

		metabuf	 = ReadBuffer(index, 0);
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
				uint32		 i;
				BlockNumber	 next_seg;

				/* Collect all pages including page index pages */
				num_pages = tp_segment_collect_pages(index, seg, &pages);

				/* Find max block number */
				for (i = 0; i < num_pages; i++)
				{
					/* max_used is exclusive (block after the last used) */
					if (pages[i] + 1 > max_used)
						max_used = pages[i] + 1;
				}

				/* Get next segment before freeing pages array */
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
					 "Final truncation: max_used=%u, nblocks=%u, saving %u "
					 "pages",
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

			worker_states[i].buffers[j].num_docs	   = 0;
			worker_states[i].buffers[j].num_terms	   = 0;
			worker_states[i].buffers[j].total_len	   = 0;
			worker_states[i].buffers[j].total_postings = 0;
			worker_states[i].buffers[j].memory_used	   = 0;
		}
	}
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

		/*
		 * Check if we need to switch buffers.
		 * Use posting count threshold to match serial build's segment sizes.
		 * Serial uses tp_memtable_spill_threshold (default 32M postings).
		 */
		if (buffer->total_postings >= tp_memtable_spill_threshold)
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
 * Merge source for streaming N-way merge from worker buffers.
 * Tracks current position in a buffer's sorted term array.
 */
typedef struct TpWorkerMergeSource
{
	dsa_area			   *dsa;		 /* DSA area */
	TpWorkerMemtableBuffer *buffer;		 /* The buffer being merged */
	TermInfo			   *terms;		 /* Sorted term array */
	uint32					num_terms;	 /* Total terms */
	uint32					current_idx; /* Current position */
	bool					exhausted;	 /* No more terms */
} TpWorkerMergeSource;

/*
 * Initialize a worker merge source.
 * Builds sorted term array from the buffer's string hash table.
 */
static bool
worker_merge_source_init(
		TpWorkerMergeSource	   *source,
		dsa_area			   *dsa,
		TpWorkerMemtableBuffer *buffer)
{
	memset(source, 0, sizeof(TpWorkerMergeSource));
	source->dsa		  = dsa;
	source->buffer	  = buffer;
	source->exhausted = true;

	if (buffer->num_docs == 0)
		return false;

	/* Build sorted dictionary from buffer */
	source->terms = build_dictionary_from_worker_buffer(
			dsa, buffer, &source->num_terms);

	if (source->num_terms == 0 || source->terms == NULL)
		return false;

	source->current_idx = 0;
	source->exhausted	= false;
	return true;
}

/*
 * Advance a worker merge source to the next term.
 */
static bool
worker_merge_source_advance(TpWorkerMergeSource *source)
{
	if (source->exhausted)
		return false;

	source->current_idx++;
	if (source->current_idx >= source->num_terms)
	{
		source->exhausted = true;
		return false;
	}
	return true;
}

/*
 * Close and cleanup a worker merge source.
 */
static void
worker_merge_source_close(TpWorkerMergeSource *source)
{
	if (source->terms)
	{
		tp_free_dictionary(source->terms, source->num_terms);
		source->terms = NULL;
	}
}

/*
 * Find the merge source with the lexicographically smallest current term.
 * Returns -1 if all sources are exhausted.
 */
static int
worker_merge_find_min_source(TpWorkerMergeSource *sources, int num_sources)
{
	int			min_idx	 = -1;
	const char *min_term = NULL;
	int			i;

	for (i = 0; i < num_sources; i++)
	{
		if (sources[i].exhausted)
			continue;

		if (min_idx < 0 ||
			strcmp(sources[i].terms[sources[i].current_idx].term, min_term) <
					0)
		{
			min_idx	 = i;
			min_term = sources[i].terms[sources[i].current_idx].term;
		}
	}

	return min_idx;
}

/*
 * Reference to a buffer that contains a particular term.
 */
typedef struct TpBufferTermRef
{
	int			source_idx;		 /* Index into sources array */
	dsa_pointer posting_list_dp; /* Pointer to posting list in DSA */
} TpBufferTermRef;

/*
 * Merged term info - tracks which buffers have this term.
 */
typedef struct TpMergedBufferTerm
{
	char			*term;			  /* Term text (palloc'd copy) */
	uint32			 term_len;		  /* Term length */
	TpBufferTermRef *buffer_refs;	  /* Which buffers have this term */
	uint32			 num_buffer_refs; /* Number of buffer refs */
	uint32			 buffer_refs_cap; /* Allocated capacity */
} TpMergedBufferTerm;

/*
 * Add a buffer reference to a merged term.
 */
static void
merged_buffer_term_add_ref(
		TpMergedBufferTerm *term, int source_idx, dsa_pointer posting_list_dp)
{
	if (term->num_buffer_refs >= term->buffer_refs_cap)
	{
		uint32 new_cap = term->buffer_refs_cap == 0
							   ? 4
							   : term->buffer_refs_cap * 2;
		if (term->buffer_refs == NULL)
			term->buffer_refs = palloc(new_cap * sizeof(TpBufferTermRef));
		else
			term->buffer_refs = repalloc(
					term->buffer_refs, new_cap * sizeof(TpBufferTermRef));
		term->buffer_refs_cap = new_cap;
	}

	term->buffer_refs[term->num_buffer_refs].source_idx		 = source_idx;
	term->buffer_refs[term->num_buffer_refs].posting_list_dp = posting_list_dp;
	term->num_buffer_refs++;
}

/*
 * Posting info during N-way merge of postings from multiple buffers.
 */
typedef struct TpBufferPostingInfo
{
	ItemPointerData ctid;
	uint16			frequency;
} TpBufferPostingInfo;

/*
 * Posting merge source for streaming from a single buffer's posting list.
 */
typedef struct TpBufferPostingSource
{
	dsa_area	   *dsa;
	TpPostingList  *posting_list;
	TpPostingEntry *entries;
	uint32			current_idx;
	uint32			doc_count;
	bool			exhausted;
} TpBufferPostingSource;

/*
 * Initialize a posting source for a buffer's posting list.
 */
static void
buffer_posting_source_init(
		TpBufferPostingSource *ps, dsa_area *dsa, dsa_pointer posting_list_dp)
{
	memset(ps, 0, sizeof(TpBufferPostingSource));
	ps->dsa		  = dsa;
	ps->exhausted = true;

	if (!DsaPointerIsValid(posting_list_dp))
		return;

	ps->posting_list = dsa_get_address(dsa, posting_list_dp);
	if (!ps->posting_list || ps->posting_list->doc_count == 0)
		return;

	if (!DsaPointerIsValid(ps->posting_list->entries_dp))
		return;

	ps->entries	  = dsa_get_address(dsa, ps->posting_list->entries_dp);
	ps->doc_count = ps->posting_list->doc_count;
	ps->exhausted = false;
}

/*
 * Advance a buffer posting source.
 */
static bool
buffer_posting_source_advance(TpBufferPostingSource *ps)
{
	if (ps->exhausted)
		return false;

	ps->current_idx++;
	if (ps->current_idx >= ps->doc_count)
	{
		ps->exhausted = true;
		return false;
	}
	return true;
}

/*
 * Find the posting source with the smallest current CTID.
 */
static int
find_min_buffer_posting_source(TpBufferPostingSource *sources, int num_sources)
{
	int				min_idx = -1;
	ItemPointerData min_ctid;
	int				i;

	for (i = 0; i < num_sources; i++)
	{
		ItemPointerData ctid;

		if (sources[i].exhausted)
			continue;

		memcpy(&ctid,
			   &sources[i].entries[sources[i].current_idx].ctid,
			   sizeof(ItemPointerData));

		if (min_idx < 0 || ItemPointerCompare(&ctid, &min_ctid) < 0)
		{
			min_idx = i;
			memcpy(&min_ctid, &ctid, sizeof(ItemPointerData));
		}
	}

	return min_idx;
}

/*
 * Collected posting for output (used during N-way posting merge).
 */
typedef struct TpCollectedBufferPosting
{
	ItemPointerData ctid;
	uint16			frequency;
} TpCollectedBufferPosting;

/*
 * Collect postings for a term using N-way merge across multiple buffers.
 */
static TpCollectedBufferPosting *
collect_buffer_term_postings(
		TpMergedBufferTerm *term, TpWorkerMergeSource *sources, uint32 *count)
{
	TpBufferPostingSource	 *psources;
	TpCollectedBufferPosting *postings;
	uint32					  capacity = 64;
	uint32					  num	   = 0;
	uint32					  i;

	*count = 0;

	if (term->num_buffer_refs == 0)
		return NULL;

	/* Initialize posting sources */
	psources = palloc(sizeof(TpBufferPostingSource) * term->num_buffer_refs);
	for (i = 0; i < term->num_buffer_refs; i++)
	{
		TpBufferTermRef		*ref	= &term->buffer_refs[i];
		TpWorkerMergeSource *source = &sources[ref->source_idx];

		buffer_posting_source_init(
				&psources[i], source->dsa, ref->posting_list_dp);
	}

	postings = palloc(sizeof(TpCollectedBufferPosting) * capacity);

	/* N-way merge: find min CTID, collect, advance */
	while (true)
	{
		int min_idx = find_min_buffer_posting_source(
				psources, term->num_buffer_refs);
		if (min_idx < 0)
			break;

		/* Grow array if needed */
		if (num >= capacity)
		{
			capacity *= 2;
			postings = repalloc(
					postings, sizeof(TpCollectedBufferPosting) * capacity);
		}

		/* Copy posting */
		memcpy(&postings[num].ctid,
			   &psources[min_idx].entries[psources[min_idx].current_idx].ctid,
			   sizeof(ItemPointerData));
		postings[num].frequency =
				psources[min_idx]
						.entries[psources[min_idx].current_idx]
						.frequency;
		num++;

		buffer_posting_source_advance(&psources[min_idx]);
	}

	pfree(psources);

	*count = num;
	return postings;
}

/*
 * Build merged docmap from multiple worker buffers.
 * Iterates through all document lengths from all buffers.
 */
static TpDocMapBuilder *
build_merged_docmap_from_buffers(
		dsa_area *dsa, TpWorkerMergeSource *sources, int num_sources)
{
	TpDocMapBuilder	 *docmap;
	dshash_parameters params;
	int				  i;

	docmap = tp_docmap_create();

	params.key_size			= sizeof(ItemPointerData);
	params.entry_size		= sizeof(TpDocLengthEntry);
	params.hash_function	= dshash_memhash;
	params.compare_function = dshash_memcmp;
	params.copy_function	= dshash_memcpy;
	params.tranche_id		= TP_DOCLENGTH_HASH_TRANCHE_ID;

	for (i = 0; i < num_sources; i++)
	{
		TpWorkerMemtableBuffer *buffer = sources[i].buffer;
		dshash_table		   *doclength_table;
		dshash_seq_status		seq_status;
		TpDocLengthEntry	   *doc_entry;

		if (buffer->doc_lengths_handle == 0)
			continue;

		doclength_table =
				dshash_attach(dsa, &params, buffer->doc_lengths_handle, dsa);

		dshash_seq_init(&seq_status, doclength_table, false);
		while ((doc_entry = (TpDocLengthEntry *)dshash_seq_next(
						&seq_status)) != NULL)
		{
			tp_docmap_add(
					docmap, &doc_entry->ctid, (uint32)doc_entry->doc_length);
		}
		dshash_seq_term(&seq_status);
		dshash_detach(doclength_table);
	}

	tp_docmap_finalize(docmap);
	return docmap;
}

/*
 * Per-term block information for segment writing
 */
typedef struct MergeTermBlockInfo
{
	uint32 posting_offset;	 /* Absolute offset where postings were written */
	uint16 block_count;		 /* Number of blocks for this term */
	uint32 doc_freq;		 /* Document frequency */
	uint32 skip_entry_start; /* Index into accumulated skip entries array */
} MergeTermBlockInfo;

/*
 * Write a segment by streaming N-way merge directly from worker buffers.
 *
 * This is the core function that merges multiple worker buffers into a single
 * segment WITHOUT copying to an intermediate local memtable. Postings are
 * streamed directly from DSA memory to disk.
 */
static BlockNumber
tp_merge_worker_buffers_to_segment(
		TpParallelBuildShared	*shared,
		TpWorkerMemtableBuffer **buffers,
		int						 num_buffers,
		Relation				 index,
		dsa_area				*dsa)
{
	TpWorkerMergeSource *sources;
	int					 num_sources	  = 0;
	TpMergedBufferTerm	*merged_terms	  = NULL;
	uint32				 num_merged_terms = 0;
	uint32				 merged_capacity  = 0;
	TpDocMapBuilder		*docmap;
	BlockNumber			 header_block;
	BlockNumber			 page_index_root;
	TpSegmentWriter		 writer;
	TpSegmentHeader		 header;
	TpDictionary		 dict;
	uint32				*string_offsets;
	uint32				 string_pos;
	uint32				 i;
	Buffer				 header_buf;
	Page				 header_page;
	TpSegmentHeader		*existing_header;
	MergeTermBlockInfo	*term_blocks;
	TpSkipEntry			*all_skip_entries;
	uint32				 skip_entries_count;
	uint32				 skip_entries_capacity;
	uint32				 total_docs = 0;
	int64				 total_len	= 0;

	if (num_buffers == 0)
		return InvalidBlockNumber;

	/* Initialize merge sources from buffers */
	sources = palloc0(sizeof(TpWorkerMergeSource) * num_buffers);
	for (i = 0; i < (uint32)num_buffers; i++)
	{
		if (worker_merge_source_init(&sources[num_sources], dsa, buffers[i]))
		{
			total_docs += buffers[i]->num_docs;
			total_len += buffers[i]->total_len;
			num_sources++;
		}
	}

	if (num_sources == 0)
	{
		pfree(sources);
		return InvalidBlockNumber;
	}

	/* Build merged docmap from all buffers */
	docmap = build_merged_docmap_from_buffers(dsa, sources, num_sources);

	/*
	 * N-way merge of terms: find min term across all sources, collect which
	 * sources have it, advance those sources.
	 */
	while (true)
	{
		int					min_idx;
		const char		   *min_term;
		TpMergedBufferTerm *current_merged;

		min_idx = worker_merge_find_min_source(sources, num_sources);
		if (min_idx < 0)
			break; /* All sources exhausted */

		min_term = sources[min_idx].terms[sources[min_idx].current_idx].term;

		/* Grow merged terms array if needed */
		if (num_merged_terms >= merged_capacity)
		{
			merged_capacity = merged_capacity == 0 ? 1024
												   : merged_capacity * 2;
			if (merged_terms == NULL)
				merged_terms = palloc(
						merged_capacity * sizeof(TpMergedBufferTerm));
			else
				merged_terms = repalloc(
						merged_terms,
						merged_capacity * sizeof(TpMergedBufferTerm));
		}

		/* Initialize new merged term */
		current_merged					= &merged_terms[num_merged_terms];
		current_merged->term			= pstrdup(min_term);
		current_merged->term_len		= strlen(min_term);
		current_merged->buffer_refs		= NULL;
		current_merged->num_buffer_refs = 0;
		current_merged->buffer_refs_cap = 0;
		num_merged_terms++;

		/* Record which sources have this term */
		for (i = 0; i < (uint32)num_sources; i++)
		{
			if (sources[i].exhausted)
				continue;

			if (strcmp(sources[i].terms[sources[i].current_idx].term,
					   current_merged->term) == 0)
			{
				/* Record buffer ref */
				merged_buffer_term_add_ref(
						current_merged,
						i,
						sources[i]
								.terms[sources[i].current_idx]
								.posting_list_dp);

				/* Advance this source */
				worker_merge_source_advance(&sources[i]);
			}
		}
	}

	if (num_merged_terms == 0)
	{
		/* Cleanup and return */
		for (i = 0; i < (uint32)num_sources; i++)
			worker_merge_source_close(&sources[i]);
		pfree(sources);
		tp_docmap_destroy(docmap);
		return InvalidBlockNumber;
	}

	/* Initialize segment writer - will extend relation as needed */
	tp_segment_writer_init(&writer, index);

	if (writer.pages_allocated > 0)
	{
		header_block = writer.pages[0];
	}
	else
	{
		elog(ERROR,
			 "tp_merge_worker_buffers_to_segment: "
			 "Failed to allocate first page");
	}

	/* Initialize header */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_VERSION;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= 0;
	header.num_terms	= num_merged_terms;
	header.level		= 0;
	header.next_segment = InvalidBlockNumber;

	/* Dictionary immediately follows header */
	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Corpus statistics from accumulated buffer data */
	header.num_docs		= docmap->num_docs;
	header.total_tokens = total_len;

	/* Write placeholder header */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary section */
	dict.num_terms = num_merged_terms;
	tp_segment_writer_write(
			&writer, &dict, offsetof(TpDictionary, string_offsets));

	/* Build string offsets */
	string_offsets = palloc0(num_merged_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_merged_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + merged_terms[i].term_len +
					  sizeof(uint32);
	}

	/* Write string offsets array */
	tp_segment_writer_write(
			&writer, string_offsets, num_merged_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = writer.current_offset;
	for (i = 0; i < num_merged_terms; i++)
	{
		uint32 length	   = merged_terms[i].term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, merged_terms[i].term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Record entries offset */
	header.entries_offset = writer.current_offset;

	/* Write placeholder dict entries */
	{
		TpDictEntry placeholder;
		memset(&placeholder, 0, sizeof(TpDictEntry));
		for (i = 0; i < num_merged_terms; i++)
			tp_segment_writer_write(
					&writer, &placeholder, sizeof(TpDictEntry));
	}

	/* Postings start here */
	header.postings_offset = writer.current_offset;

	/* Initialize per-term tracking and skip entry accumulator */
	term_blocks = palloc0(num_merged_terms * sizeof(MergeTermBlockInfo));

	skip_entries_capacity = 1024;
	skip_entries_count	  = 0;
	all_skip_entries = palloc(skip_entries_capacity * sizeof(TpSkipEntry));

	/*
	 * Streaming pass: for each term, collect postings via N-way merge and
	 * write immediately.
	 */
	for (i = 0; i < num_merged_terms; i++)
	{
		TpCollectedBufferPosting *postings;
		uint32					  doc_count = 0;
		uint32					  j;
		uint32					  block_idx;
		uint32					  num_blocks;
		TpBlockPosting			 *block_postings;

		/* Record where this term's postings start */
		term_blocks[i].posting_offset	= writer.current_offset;
		term_blocks[i].skip_entry_start = skip_entries_count;

		/* Collect postings for this term via N-way merge */
		postings = collect_buffer_term_postings(
				&merged_terms[i], sources, &doc_count);
		term_blocks[i].doc_freq = doc_count;

		if (doc_count == 0 || postings == NULL)
		{
			term_blocks[i].block_count = 0;
			continue;
		}

		num_blocks = (doc_count + TP_BLOCK_SIZE - 1) / TP_BLOCK_SIZE;
		term_blocks[i].block_count = (uint16)num_blocks;

		/* Convert postings to block format */
		block_postings = palloc(doc_count * sizeof(TpBlockPosting));
		for (j = 0; j < doc_count; j++)
		{
			uint32 doc_id = tp_docmap_lookup(docmap, &postings[j].ctid);
			uint8  norm;

			if (doc_id == UINT32_MAX)
			{
				elog(ERROR,
					 "CTID (%u,%u) not found in docmap",
					 ItemPointerGetBlockNumber(&postings[j].ctid),
					 ItemPointerGetOffsetNumber(&postings[j].ctid));
			}

			norm = tp_docmap_get_fieldnorm(docmap, doc_id);

			block_postings[j].doc_id	= doc_id;
			block_postings[j].frequency = postings[j].frequency;
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

			/* Build skip entry */
			skip.last_doc_id	= last_doc_id;
			skip.doc_count		= (uint8)(block_end - block_start);
			skip.block_max_tf	= max_tf;
			skip.block_max_norm = max_norm;
			skip.posting_offset = writer.current_offset;
			memset(skip.reserved, 0, sizeof(skip.reserved));

			/* Write posting block data (compressed or uncompressed) */
			if (tp_compress_segments)
			{
				uint8  compressed_buf[TP_MAX_COMPRESSED_BLOCK_SIZE];
				uint32 compressed_size;

				compressed_size = tp_compress_block(
						&block_postings[block_start],
						block_end - block_start,
						compressed_buf);

				skip.flags = TP_BLOCK_FLAG_DELTA;
				tp_segment_writer_write(
						&writer, compressed_buf, compressed_size);
			}
			else
			{
				skip.flags = TP_BLOCK_FLAG_UNCOMPRESSED;
				tp_segment_writer_write(
						&writer,
						&block_postings[block_start],
						(block_end - block_start) * sizeof(TpBlockPosting));
			}

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
		pfree(postings);

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

	/* Write CTID arrays - use split storage */
	header.ctid_pages_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_pages,
				docmap->num_docs * sizeof(BlockNumber));
	}

	header.ctid_offsets_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer,
				docmap->ctid_offsets,
				docmap->num_docs * sizeof(OffsetNumber));
	}

	/* Write fieldnorms */
	header.fieldnorm_offset = writer.current_offset;
	if (docmap->num_docs > 0)
	{
		tp_segment_writer_write(
				&writer, docmap->fieldnorms, docmap->num_docs * sizeof(uint8));
	}

	/* Flush writer before writing page index and dict entries */
	tp_segment_writer_flush(&writer);

	/*
	 * Mark buffer as empty to prevent tp_segment_writer_finish from flushing
	 * again and overwriting our dict entry updates.
	 */
	writer.buffer_pos = SizeOfPageHeaderData;

	/* Write page index (extends relation as needed) */
	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);
	header.page_index = page_index_root;

	/* Finalize header */
	header.num_pages = writer.pages_allocated;

	/*
	 * Write dictionary entries with correct skip_index_offset values.
	 * Entries may span multiple pages, so we need page-boundary-aware code.
	 */
	{
		Buffer dict_buf		= InvalidBuffer;
		uint32 current_page = UINT32_MAX;

		for (i = 0; i < num_merged_terms; i++)
		{
			TpDictEntry entry;
			uint32		entry_offset;
			uint32		entry_logical_page;
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

	/* Write final header */
	header_buf = ReadBuffer(index, header_block);
	LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
	header_page		= BufferGetPage(header_buf);
	existing_header = (TpSegmentHeader *)((char *)header_page +
										  SizeOfPageHeaderData);

	memcpy(existing_header, &header, sizeof(TpSegmentHeader));

	MarkBufferDirty(header_buf);
	UnlockReleaseBuffer(header_buf);

	/* Cleanup merge sources */
	for (i = 0; i < (uint32)num_sources; i++)
		worker_merge_source_close(&sources[i]);
	pfree(sources);

	/* Free merged terms */
	for (i = 0; i < num_merged_terms; i++)
	{
		if (merged_terms[i].term)
			pfree(merged_terms[i].term);
		if (merged_terms[i].buffer_refs)
			pfree(merged_terms[i].buffer_refs);
	}
	if (merged_terms)
		pfree(merged_terms);

	pfree(term_blocks);
	pfree(all_skip_entries);
	pfree(string_offsets);
	tp_docmap_destroy(docmap);
	tp_segment_writer_finish(&writer);

	return header_block;
}

/*
 * Clear a worker buffer's hash tables for reuse.
 * Destroys old tables and creates fresh empty ones.
 */
static void
clear_worker_buffer(dsa_area *dsa, TpWorkerMemtableBuffer *buffer)
{
	if (buffer->string_hash_handle != 0)
	{
		dshash_table	 *old_string_table;
		dshash_table	 *new_string_table;
		dshash_parameters string_params;

		string_params.key_size		   = sizeof(TpStringKey);
		string_params.entry_size	   = sizeof(TpStringHashEntry);
		string_params.hash_function	   = tp_worker_string_hash;
		string_params.compare_function = tp_worker_string_compare;
		string_params.copy_function	   = dshash_memcpy;
		string_params.tranche_id	   = TP_STRING_HASH_TRANCHE_ID;
		old_string_table			   = dshash_attach(
				  dsa, &string_params, buffer->string_hash_handle, dsa);
		dshash_destroy(old_string_table);

		new_string_table		   = tp_worker_create_string_table(dsa);
		buffer->string_hash_handle = dshash_get_hash_table_handle(
				new_string_table);
		dshash_detach(new_string_table);
	}

	if (buffer->doc_lengths_handle != 0)
	{
		dshash_table	 *old_doc_table;
		dshash_table	 *new_doc_table;
		dshash_parameters doc_params;

		doc_params.key_size			= sizeof(ItemPointerData);
		doc_params.entry_size		= sizeof(TpDocLengthEntry);
		doc_params.hash_function	= dshash_memhash;
		doc_params.compare_function = dshash_memcmp;
		doc_params.copy_function	= dshash_memcpy;
		doc_params.tranche_id		= TP_DOCLENGTH_HASH_TRANCHE_ID;
		old_doc_table				= dshash_attach(
				  dsa, &doc_params, buffer->doc_lengths_handle, dsa);
		dshash_destroy(old_doc_table);

		new_doc_table			   = tp_worker_create_doclength_table(dsa);
		buffer->doc_lengths_handle = dshash_get_hash_table_handle(
				new_doc_table);
		dshash_detach(new_doc_table);
	}

	/* Clear buffer stats */
	buffer->num_docs	   = 0;
	buffer->num_terms	   = 0;
	buffer->total_len	   = 0;
	buffer->total_postings = 0;
	buffer->memory_used	   = 0;
}

/*
 * Leader processes worker buffers by directly streaming N-way merge to
 * segment.
 *
 * Architecture:
 * - Workers fill buffer A, signal ready, immediately continue filling buffer B
 * - When B fills, signal ready, re-acquire A (after leader processed it)
 * - Leader waits until enough buffers are ready, then merges them directly to
 *   segment in a streaming fashion (no intermediate copy)
 * - After merge, clears processed buffers so workers can reuse them
 *
 * This produces a single compact segment similar to serial build, without
 * the memory overhead of accumulating all data into a local memtable first.
 */
static void
tp_leader_process_buffers(
		TpParallelBuildShared *shared, Relation index, dsa_area *dsa)
{
	TpWorkerState			*worker_states = TpParallelWorkerStates(shared);
	TpWorkerMemtableBuffer **ready_buffers;
	int						 num_ready;
	int						 max_ready;
	bool					 all_done	= false;
	uint32					 total_docs = 0;
	int64					 total_len	= 0;

	/* Allocate array to track ready buffers */
	max_ready	  = shared->nworkers * 2; /* 2 buffers per worker */
	ready_buffers = palloc(sizeof(TpWorkerMemtableBuffer *) * max_ready);

	while (!all_done)
	{
		int i;

		/* Collect all currently ready buffers */
		num_ready = 0;
		for (i = 0; i < shared->nworkers; i++)
		{
			int j;
			for (j = 0; j < 2; j++)
			{
				TpWorkerMemtableBuffer *buffer = &worker_states[i].buffers[j];
				uint32 status = pg_atomic_read_u32(&buffer->status);

				if (status == TP_BUFFER_READY)
				{
					ready_buffers[num_ready++] = buffer;
				}
			}
		}

		/* Check if all workers are done */
		SpinLockAcquire(&shared->mutex);
		all_done = (shared->workers_done >= shared->nworkers);
		SpinLockRelease(&shared->mutex);

		/*
		 * Merge if we have ready buffers and either:
		 * - All workers are done (final merge)
		 * - We have at least nworkers buffers ready (one per worker on
		 * average)
		 *
		 * This allows the leader to start merging even if some workers are
		 * lagging - e.g., if one worker has filled both buffers while another
		 * is slow, we can still make progress.
		 */
		if (num_ready > 0 && (all_done || num_ready >= shared->nworkers))
		{
			BlockNumber seg_block;

			elog(DEBUG1,
				 "Merging %d ready buffers (all_done=%d)",
				 num_ready,
				 all_done);

			/* Merge ready buffers directly to segment */
			seg_block = tp_merge_worker_buffers_to_segment(
					shared, ready_buffers, num_ready, index, dsa);

			if (seg_block != InvalidBlockNumber)
			{
				/* Accumulate stats from processed buffers */
				for (i = 0; i < num_ready; i++)
				{
					total_docs += ready_buffers[i]->num_docs;
					total_len += ready_buffers[i]->total_len;
				}

				/* Link segment into chain */
				if (shared->segment_head == InvalidBlockNumber)
				{
					shared->segment_head = seg_block;
					shared->segment_tail = seg_block;
				}
				else
				{
					/* Link new segment to existing chain */
					Buffer			 tail_buf;
					Page			 tail_page;
					TpSegmentHeader *tail_header;

					tail_buf = ReadBuffer(index, shared->segment_tail);
					LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
					tail_page	= BufferGetPage(tail_buf);
					tail_header = (TpSegmentHeader *)((char *)tail_page +
													  SizeOfPageHeaderData);
					tail_header->next_segment = seg_block;
					MarkBufferDirty(tail_buf);
					UnlockReleaseBuffer(tail_buf);

					shared->segment_tail = seg_block;
				}
				shared->segment_count++;
			}

			/* Clear processed buffers and signal workers */
			for (i = 0; i < num_ready; i++)
			{
				TpWorkerMemtableBuffer *buffer = ready_buffers[i];

				clear_worker_buffer(dsa, buffer);

				pg_atomic_write_u32(&buffer->status, TP_BUFFER_EMPTY);

				/* Find the worker that owns this buffer and signal it */
				{
					int w;
					for (w = 0; w < shared->nworkers; w++)
					{
						if (&worker_states[w].buffers[0] == buffer ||
							&worker_states[w].buffers[1] == buffer)
						{
							ConditionVariableSignal(
									&worker_states[w].buffer_consumed_cv);
							break;
						}
					}
				}
			}
		}
		else if (!all_done)
		{
			/* No ready buffers and not done - wait for work */
			ConditionVariableSleep(
					&shared->leader_wake_cv,
					WAIT_EVENT_PARALLEL_CREATE_INDEX_SCAN);
			ConditionVariableCancelSleep();
		}

		CHECK_FOR_INTERRUPTS();
	}

	/* Update corpus statistics in shared state */
	pg_atomic_write_u64(&shared->total_docs, total_docs);
	pg_atomic_write_u64(&shared->total_len, total_len);

	/* Cleanup */
	pfree(ready_buffers);
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

		/* Track total postings for spill threshold */
		buffer->total_postings++;

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
