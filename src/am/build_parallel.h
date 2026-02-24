/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.h - Parallel index build structures
 *
 * Architecture (two-phase with cross-worker compaction):
 * - Phase 1: Workers scan heap, flush to BufFile, compact within
 *   BufFile. Workers report segment info and signal phase1_done.
 * - Barrier: Leader plans cross-worker merge groups, sums pages,
 *   pre-extends the relation, sets atomic next_page, signals
 *   phase2.
 * - Phase 2: Workers COPY their non-merged segments to pages,
 *   then work-steal merge groups. Each task bulk-claims a
 *   contiguous page range from the shared counter.
 * - Leader truncates unused pool pages, links segments into
 *   per-level chains, updates metapage.
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
 * Maximum segments a single worker can produce after compaction.
 * With segments_per_level=8 and TP_MAX_LEVELS=8, a worker can have
 * at most 7 segments per level = 7*8 = 56, but typically far fewer.
 */
#define TP_MAX_WORKER_SEGMENTS 64

/*
 * Maximum cross-worker merge groups planned by leader.
 * Each group merges segments_per_level segments into one.
 */
#define TP_MAX_MERGE_GROUPS 256

/*
 * Shared memory keys for parallel build TOC
 */
#define TP_PARALLEL_KEY_SHARED UINT64CONST(0xB175DA7A00000001)

/*
 * Per-worker result reported back to leader via shared memory.
 */
typedef struct TpParallelWorkerResult
{
	uint32 segment_count; /* Segments written to this worker's BufFile */

	/* Corpus statistics */
	uint64 total_docs; /* Documents indexed */
	uint64 total_len;  /* Sum of document lengths */
	uint64 tuples_scanned;

	/* Per-segment info after compaction (BufFile offsets and levels) */
	uint32 final_segment_count;
	uint64 seg_offsets[TP_MAX_WORKER_SEGMENTS];
	uint64 seg_sizes[TP_MAX_WORKER_SEGMENTS];
	uint32 seg_levels[TP_MAX_WORKER_SEGMENTS];

	/*
	 * Set by leader between Phase 1 and Phase 2.
	 * 0 = COPY to pages at original level.
	 * N>0 = member of merge group N (1-indexed).
	 */
	uint32 seg_merge_group[TP_MAX_WORKER_SEGMENTS];

	/* Phase 2 → leader: segment roots written to index pages */
	BlockNumber seg_roots[TP_MAX_WORKER_SEGMENTS];
} TpParallelWorkerResult;

/*
 * Shared state for parallel index build
 *
 * Stored in DSM segment, accessible to all workers and leader.
 */
typedef struct TpParallelBuildShared
{
	/* Immutable configuration (set before workers launch) */
	Oid		   heaprelid;		  /* Heap relation OID */
	Oid		   indexrelid;		  /* Index relation OID */
	Oid		   text_config_oid;	  /* Text search configuration OID */
	AttrNumber attnum;			  /* Attribute number of indexed column */
	double	   k1;				  /* BM25 k1 parameter */
	double	   b;				  /* BM25 b parameter */
	int32	   nworkers;		  /* Number of workers requested */
	int32	   nworkers_launched; /* Actual workers launched */

	/* Per-worker heap block ranges for disjoint TID scan */
	BlockNumber		 worker_start_block[TP_MAX_PARALLEL_WORKERS];
	BlockNumber		 worker_end_block[TP_MAX_PARALLEL_WORKERS];
	pg_atomic_uint32 scan_ready; /* 1 when block ranges set */

	/* Temp files for worker segments */
	SharedFileSet fileset;

	/* Coordination */
	ConditionVariable all_done_cv; /* Workers signal when done */
	pg_atomic_uint32  workers_done;

	/* Two-phase coordination */
	pg_atomic_uint32  phase1_done;	/* Workers done with BufFile phase */
	ConditionVariable phase2_cv;	/* Leader signals pages ready */
	pg_atomic_uint32  phase2_ready; /* 1 when pages pre-allocated */
	pg_atomic_uint64  next_page;	/* Atomic page counter for Phase 2 */

	/* Progress reporting (atomic so leader can poll while workers run) */
	pg_atomic_uint64 tuples_done; /* Total tuples processed by all workers */

	/* Cross-worker merge groups (planned by leader after Phase 1) */
	uint32			 num_merge_groups;
	uint32			 merge_group_levels[TP_MAX_MERGE_GROUPS];
	BlockNumber		 merge_group_results[TP_MAX_MERGE_GROUPS];
	pg_atomic_uint32 next_merge_group;

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
