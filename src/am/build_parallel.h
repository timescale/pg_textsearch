/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am/build_parallel.h - Parallel index build structures
 *
 * Architecture:
 * - Workers scan heap and build memtables in shared DSA memory
 * - Each worker has two memtables (double-buffering) to avoid blocking
 * - Leader doesn't scan - it reads worker memtables and writes L0 segments
 * - All disk I/O is done by the leader, ensuring contiguous page allocation
 */
#pragma once

#include <postgres.h>

#include <access/parallel.h>
#include <lib/dshash.h>
#include <storage/block.h>
#include <storage/condition_variable.h>
#include <storage/spin.h>
#include <utils/dsa.h>
#include <utils/relcache.h>

/*
 * Maximum parallel workers for index build.
 * Should not exceed max_parallel_maintenance_workers.
 */
#define TP_MAX_PARALLEL_WORKERS 32

/*
 * Shared memory keys for parallel build TOC
 */
#define TP_PARALLEL_KEY_SHARED	   UINT64CONST(0xB175DA7A00000001)
#define TP_PARALLEL_KEY_TABLE_SCAN UINT64CONST(0xB175DA7A00000004)

/*
 * Worker memtable buffer status
 */
typedef enum TpWorkerBufferStatus
{
	TP_BUFFER_EMPTY = 0, /* Buffer available for worker to fill */
	TP_BUFFER_FILLING,	 /* Worker is currently filling this buffer */
	TP_BUFFER_READY,	 /* Buffer has data, leader can process it */
	TP_BUFFER_DONE		 /* Worker is done, no more data coming */
} TpWorkerBufferStatus;

/*
 * Per-worker memtable buffer info
 *
 * Each worker has two buffers for double-buffering. The worker fills one
 * while the leader processes the other.
 */
typedef struct TpWorkerMemtableBuffer
{
	pg_atomic_uint32 status; /* TpWorkerBufferStatus */

	/* Memtable data (stored in shared DSA) */
	dshash_table_handle string_hash_handle; /* Term -> posting list */
	dshash_table_handle doc_lengths_handle; /* CTID -> doc length */

	/* Statistics for this buffer */
	int32 num_docs;		  /* Documents in this buffer */
	int32 num_terms;	  /* Unique terms in this buffer */
	int64 total_len;	  /* Sum of document lengths */
	int64 total_postings; /* Total posting entries (for spill threshold) */
	Size  memory_used;	  /* Approximate memory used */
} TpWorkerMemtableBuffer;

/*
 * Per-worker state in shared memory
 */
typedef struct TpWorkerState
{
	/* Double-buffered memtables */
	TpWorkerMemtableBuffer buffers[2];

	/* Which buffer the worker is currently filling (0 or 1) */
	pg_atomic_uint32 active_buffer;

	/* Signaling */
	ConditionVariable buffer_ready_cv;	  /* Worker signals: buffer ready */
	ConditionVariable buffer_consumed_cv; /* Leader signals: buffer consumed */

	/* Worker status */
	pg_atomic_uint32 scan_complete;	 /* Worker finished scanning */
	pg_atomic_uint64 tuples_scanned; /* Progress counter */
} TpWorkerState;

/*
 * Shared state for parallel index build
 *
 * Stored in DSM segment, accessible to all workers and leader.
 */
typedef struct TpParallelBuildShared
{
	/* Immutable configuration (set before workers launch) */
	Oid		   heaprelid;		/* Heap relation OID */
	Oid		   indexrelid;		/* Index relation OID */
	Oid		   text_config_oid; /* Text search configuration OID */
	AttrNumber attnum;			/* Attribute number of indexed column */
	double	   k1;				/* BM25 k1 parameter */
	double	   b;				/* BM25 b parameter */
	int32	   nworkers;		/* Number of background workers (not including
								   leader) */

	/* Per-worker memory budget for memtable (in bytes) */
	Size memory_budget_per_worker;

	/* DSA for shared memtable allocations */
	dsa_handle memtable_dsa_handle; /* Handle for attaching to DSA */

	/* Leader coordination */
	slock_t			  mutex;		  /* Protects mutable scalar fields */
	ConditionVariable leader_wake_cv; /* Wake leader when work available */
	int32			  workers_done;	  /* Number of workers finished */

	/* Page pool for segment writes (leader only) */
	int32			 total_pool_pages; /* Total pre-allocated pages */
	pg_atomic_uint32 pool_next;		   /* Next page index */

	/* Aggregate statistics */
	pg_atomic_uint64 total_docs; /* Total documents indexed */
	pg_atomic_uint64 total_len;	 /* Sum of all document lengths */

	/* L0 segment tracking (leader writes these) */
	BlockNumber segment_head;  /* First L0 segment */
	BlockNumber segment_tail;  /* Last L0 segment (for chaining) */
	int32		segment_count; /* Number of L0 segments written */

	/*
	 * Variable-length data follows:
	 * - TpWorkerState worker_states[nworkers]
	 * - BlockNumber page_pool[total_pool_pages]
	 * - ParallelTableScanDescData (for parallel heap scan)
	 */
} TpParallelBuildShared;

/*
 * Get pointer to worker state array
 */
static inline TpWorkerState *
TpParallelWorkerStates(TpParallelBuildShared *shared)
{
	return (TpWorkerState *)((char *)shared +
							 MAXALIGN(sizeof(TpParallelBuildShared)));
}

/*
 * Get pointer to page pool
 */
static inline BlockNumber *
TpParallelPagePool(TpParallelBuildShared *shared)
{
	char *base = (char *)TpParallelWorkerStates(shared);
	base += MAXALIGN(sizeof(TpWorkerState) * shared->nworkers);
	return (BlockNumber *)base;
}

/*
 * Get pointer to parallel table scan descriptor
 */
static inline ParallelTableScanDesc
TpParallelTableScan(TpParallelBuildShared *shared)
{
	char *base = (char *)TpParallelWorkerStates(shared);
	base += MAXALIGN(sizeof(TpWorkerState) * shared->nworkers);
	base += MAXALIGN((size_t)shared->total_pool_pages * sizeof(BlockNumber));
	return (ParallelTableScanDesc)base;
}

/*
 * Function declarations
 */

/* Main parallel build entry point */
extern struct IndexBuildResult *tp_build_parallel(
		Relation		  heap,
		Relation		  index,
		struct IndexInfo *indexInfo,
		Oid				  text_config_oid,
		double			  k1,
		double			  b,
		int				  nworkers);

/* Worker entry point (called by parallel infrastructure) */
extern PGDLLEXPORT void
tp_parallel_build_worker_main(dsm_segment *seg, shm_toc *toc);

/* Page pool allocation during segment write */
extern BlockNumber tp_pool_get_page(TpParallelBuildShared *shared);

/* Estimate shared memory size needed for parallel build */
extern Size tp_parallel_build_estimate_shmem(
		Relation heap, Snapshot snapshot, int nworkers, int total_pool_pages);
