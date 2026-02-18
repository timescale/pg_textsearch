/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.h - Parallel index build structures
 *
 * Architecture:
 * - Workers scan heap and build local TpBuildContext (arena + HTAB)
 * - When budget fills, workers flush segments directly to index pages
 * - Each worker chains its own segments locally
 * - After all workers finish, leader links chains into L0, compacts
 * - No DSA, no dshash, no double-buffering, no leader-side merging
 */
#pragma once

#include <postgres.h>

#include <access/parallel.h>
#include <storage/block.h>
#include <storage/condition_variable.h>
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
 * Per-worker result reported back to leader via shared memory.
 */
typedef struct TpParallelWorkerResult
{
	/* Segment chain written by this worker */
	BlockNumber segment_head;  /* First segment (newest) */
	BlockNumber segment_tail;  /* Last segment (oldest, for linking) */
	int32		segment_count; /* Number of segments */

	/* Corpus statistics */
	uint64 total_docs; /* Documents indexed */
	uint64 total_len;  /* Sum of document lengths */
	uint64 tuples_scanned;
} TpParallelWorkerResult;

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
	int32	   nworkers;		/* Number of workers requested */

	/* Pre-allocated page pool for workers */
	BlockNumber		 pool_start; /* First block of pool */
	uint32			 pool_size;	 /* Total pages in pool */
	pg_atomic_uint32 pool_next;	 /* Next page index (atomic) */

	/* Coordination */
	ConditionVariable all_done_cv; /* Workers signal when done */
	pg_atomic_uint32  workers_done;

	/* Progress reporting (atomic so leader can poll while workers run) */
	pg_atomic_uint64 tuples_done; /* Total tuples processed by all workers */

	/*
	 * Per-worker results (variable-length array follows).
	 * Workers write their own slot; leader reads after completion.
	 */
} TpParallelBuildShared;

/*
 * Get pointer to worker results array
 */
static inline TpParallelWorkerResult *
TpParallelWorkerResults(TpParallelBuildShared *shared)
{
	return (TpParallelWorkerResult *)((char *)shared +
									  MAXALIGN(sizeof(TpParallelBuildShared)));
}

/*
 * Get pointer to parallel table scan descriptor
 */
static inline ParallelTableScanDesc
TpParallelTableScan(TpParallelBuildShared *shared)
{
	char *base = (char *)TpParallelWorkerResults(shared);
	base += MAXALIGN(sizeof(TpParallelWorkerResult) * shared->nworkers);
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

/* Estimate shared memory size needed for parallel build */
extern Size tp_parallel_build_estimate_shmem(
		Relation heap, Snapshot snapshot, int nworkers);
