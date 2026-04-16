/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.h - Parallel index build structures
 *
 * Architecture (leader-only merge):
 * - Phase 1: Workers scan heap, flush L0 segments to BufFile.
 *   Workers report segment offsets/sizes and signal phase1_done.
 * - Leader: Opens all worker BufFiles, performs single N-way merge
 *   directly to paged storage. Updates metapage.
 * - Leader signals phase2_ready; workers wake and exit.
 */
#pragma once

#include <postgres.h>

#include <access/parallel.h>
#include <storage/block.h>
#include <storage/condition_variable.h>
#include <storage/sharedfileset.h>
#include <utils/relcache.h>

/*
 * Maximum parallel workers for index build.
 * Should not exceed max_parallel_maintenance_workers.
 */
#define TP_MAX_PARALLEL_WORKERS 32

/*
 * Maximum L0 segments a single worker can produce.
 * Without worker-side compaction, this is bounded by
 * (table_size / maintenance_work_mem_per_worker).
 */
#define TP_MAX_WORKER_SEGMENTS 64

/*
 * Shared memory keys for parallel build TOC
 */
#define TP_PARALLEL_KEY_SHARED UINT64CONST(0xB175DA7A00000001)

/*
 * Per-worker result reported back to leader via shared memory.
 */
typedef struct TpParallelWorkerResult
{
	/* Corpus statistics */
	uint64 total_docs; /* Documents indexed */
	uint64 total_len;  /* Sum of document lengths */
	uint64 tuples_scanned;

	/* Per-segment info (all L0, BufFile offsets) */
	uint32 final_segment_count;
	uint64 seg_offsets[TP_MAX_WORKER_SEGMENTS];
	uint64 seg_sizes[TP_MAX_WORKER_SEGMENTS];
} TpParallelWorkerResult;

/*
 * Shared state for parallel index build
 *
 * Stored in DSM segment, accessible to all workers and leader.
 */
typedef struct TpParallelBuildShared
{
	/* Immutable configuration (set before workers launch) */
	Oid	   heaprelid;		  /* Heap relation OID */
	Oid	   indexrelid;		  /* Index relation OID */
	Oid	   text_config_oid;	  /* Text search config OID */
	double k1;				  /* BM25 k1 parameter */
	double b;				  /* BM25 b parameter */
	bool   is_text_array;	  /* Indexed column is text[] */
	int32  nworkers;		  /* Workers requested */
	int32  nworkers_launched; /* Actual workers launched */

	/* Per-worker heap block ranges for disjoint TID scan */
	BlockNumber		 worker_start_block[TP_MAX_PARALLEL_WORKERS];
	BlockNumber		 worker_end_block[TP_MAX_PARALLEL_WORKERS];
	pg_atomic_uint32 scan_ready; /* 1 when block ranges set */

	/* Temp files for worker segments */
	SharedFileSet fileset;

	/* Coordination */
	ConditionVariable all_done_cv; /* Workers signal when done */
	pg_atomic_uint32  workers_done;

	/* Phase coordination */
	pg_atomic_uint32  phase1_done;	/* Workers done with BufFile */
	ConditionVariable phase2_cv;	/* Leader signals merge done */
	pg_atomic_uint32  phase2_ready; /* 1 when leader merge done */

	/* Progress reporting */
	pg_atomic_uint64 tuples_done;

	/*
	 * Per-worker results (variable-length array follows).
	 * Workers write their own slot; leader reads after Phase 1.
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
		bool			  is_text_array,
		int				  nworkers);

/* Worker entry point (called by parallel infrastructure) */
extern PGDLLEXPORT void
tp_parallel_build_worker_main(dsm_segment *seg, shm_toc *toc);

/* Estimate shared memory size needed for parallel build */
extern Size tp_parallel_build_estimate_shmem(
		Relation heap, Snapshot snapshot, int nworkers);
