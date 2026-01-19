/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am/build_parallel.h - Parallel index build structures
 */
#pragma once

#include <postgres.h>

#include <access/parallel.h>
#include <storage/block.h>
#include <storage/condition_variable.h>
#include <storage/spin.h>
#include <utils/relcache.h>

/*
 * Maximum parallel workers for index build.
 * Should not exceed max_parallel_maintenance_workers.
 */
#define TP_MAX_PARALLEL_WORKERS 32

/*
 * Shared memory keys for parallel build TOC
 */
#define TP_PARALLEL_KEY_SHARED		 UINT64CONST(0xB175DA7A00000001)
#define TP_PARALLEL_KEY_WORKER_INFO	 UINT64CONST(0xB175DA7A00000002)
#define TP_PARALLEL_KEY_PAGE_POOLS	 UINT64CONST(0xB175DA7A00000003)
#define TP_PARALLEL_KEY_TABLE_SCAN	 UINT64CONST(0xB175DA7A00000004)
#define TP_PARALLEL_KEY_WAL_USAGE	 UINT64CONST(0xB175DA7A00000005)
#define TP_PARALLEL_KEY_BUFFER_USAGE UINT64CONST(0xB175DA7A00000006)

/*
 * Per-worker segment tracking
 *
 * Each worker maintains its own chain of segments. After the build completes,
 * the leader links all worker chains into L0.
 */
typedef struct TpWorkerSegmentInfo
{
	BlockNumber segment_head;  /* First segment written by this worker */
	BlockNumber segment_tail;  /* Last segment (for chaining) */
	int32		segment_count; /* Number of segments written */

	/* Statistics accumulated by this worker */
	int64 docs_indexed; /* Documents processed */
	int64 total_len;	/* Sum of document lengths */
} TpWorkerSegmentInfo;

/*
 * Shared state for parallel index build
 *
 * Stored in DSM segment, accessible to all workers and leader.
 * All mutable fields must be accessed atomically or under mutex.
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
	int32	   worker_count;	/* Total workers (including leader) */

	/* Per-worker memory budget for memtable (in bytes) */
	Size memory_budget_per_worker;

	/* Worker coordination */
	slock_t			  mutex;		 /* Protects mutable scalar fields */
	ConditionVariable workersdonecv; /* Signaled when worker completes */
	int32			  workers_done;	 /* Number of workers finished */
	bool leader_working; /* True if leader participates as worker */

	/* Aggregate statistics (updated atomically) */
	pg_atomic_uint64 tuples_scanned; /* Progress counter */
	pg_atomic_uint64 total_docs;	 /* Total documents indexed */
	pg_atomic_uint64 total_len;		 /* Sum of all document lengths */

	/* Page pool management - single shared pool for all workers */
	int32			 total_pool_pages; /* Total pre-allocated pages */
	pg_atomic_uint32 shared_pool_next; /* Next page index (shared counter) */
	pg_atomic_uint32 pool_exhausted;   /* Non-zero if pool exhausted */
	pg_atomic_uint32 max_block_used;   /* Highest block number used */

	/*
	 * Variable-length data follows:
	 * - TpWorkerSegmentInfo worker_info[worker_count]
	 * - BlockNumber page_pool[total_pool_pages] (shared by all workers)
	 * - ParallelTableScanDescData (for parallel heap scan)
	 */
} TpParallelBuildShared;

/*
 * Get pointer to worker info array
 */
static inline TpWorkerSegmentInfo *
TpParallelWorkerInfo(TpParallelBuildShared *shared)
{
	return (TpWorkerSegmentInfo *)((char *)shared +
								   MAXALIGN(sizeof(TpParallelBuildShared)));
}

/*
 * Get pointer to shared page pool
 */
static inline BlockNumber *
TpParallelPagePool(TpParallelBuildShared *shared)
{
	char *base = (char *)TpParallelWorkerInfo(shared);
	base += MAXALIGN(sizeof(TpWorkerSegmentInfo) * shared->worker_count);
	return (BlockNumber *)base;
}

/*
 * Get pointer to parallel table scan descriptor
 */
static inline ParallelTableScanDesc
TpParallelTableScan(TpParallelBuildShared *shared)
{
	char *base = (char *)TpParallelWorkerInfo(shared);
	base += MAXALIGN(sizeof(TpWorkerSegmentInfo) * shared->worker_count);
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
extern BlockNumber
tp_pool_get_page(TpParallelBuildShared *shared, int worker_id, Relation index);

/* Estimate shared memory size needed for parallel build */
extern Size tp_parallel_build_estimate_shmem(
		Relation heap, Snapshot snapshot, int nworkers, int total_pool_pages);
