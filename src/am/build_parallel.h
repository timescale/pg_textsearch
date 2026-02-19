/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.h - Parallel index build structures
 *
 * Architecture (two-phase):
 * - Phase 1: Workers scan heap, flush to BufFile, compact within BufFile.
 *   Each worker reports total_pages_needed and signals phase1_done.
 * - Barrier: Leader sums pages, pre-extends the relation with
 *   ExtendBufferedRelBy, sets atomic next_page counter, signals phase2.
 * - Phase 2: Workers reopen their BufFile read-only and write segments
 *   directly to pre-allocated index pages using atomic page counter.
 *   Workers report seg_roots[]/seg_levels[] to shared memory.
 * - Leader reads seg_roots from workers (no BufFile I/O), chains
 *   segments into level lists, updates metapage, runs final compaction.
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
 * Shared memory keys for parallel build TOC
 */
#define TP_PARALLEL_KEY_SHARED	   UINT64CONST(0xB175DA7A00000001)
#define TP_PARALLEL_KEY_TABLE_SCAN UINT64CONST(0xB175DA7A00000004)

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

	/* Phase 1 → leader: total index pages this worker needs */
	uint64 total_pages_needed;

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
	Oid		   heaprelid;		/* Heap relation OID */
	Oid		   indexrelid;		/* Index relation OID */
	Oid		   text_config_oid; /* Text search configuration OID */
	AttrNumber attnum;			/* Attribute number of indexed column */
	double	   k1;				/* BM25 k1 parameter */
	double	   b;				/* BM25 b parameter */
	int32	   nworkers;		/* Number of workers requested */

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
