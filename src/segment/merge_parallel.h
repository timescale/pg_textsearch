/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment/merge_parallel.h - Parallel segment compaction structures
 */
#pragma once

#include <postgres.h>

#include <access/parallel.h>
#include <port/atomics.h>
#include <storage/block.h>
#include <storage/condition_variable.h>
#include <storage/spin.h>
#include <utils/relcache.h>

#include "constants.h"

/*
 * Maximum workers for parallel compaction.
 * Limited by max_parallel_maintenance_workers GUC.
 */
#define TP_MAX_COMPACT_WORKERS 32

/*
 * Maximum segments that can be merged in a single task.
 * Typically segments_per_level + 1 for runt handling.
 */
#define TP_MAX_SEGMENTS_PER_TASK (TP_DEFAULT_SEGMENTS_PER_LEVEL + 2)

/*
 * Shared memory key for parallel compaction TOC
 */
#define TP_PARALLEL_COMPACT_KEY_SHARED UINT64CONST(0xB175DA7A00000010)

/*
 * Compaction task - describes a single N-way merge operation.
 *
 * Each task merges segments from source_level into a single segment
 * at target_level. Workers pull tasks from a shared queue.
 */
typedef struct TpCompactTask
{
	uint32		source_level;
	uint32		target_level;
	BlockNumber source_segments[TP_MAX_SEGMENTS_PER_TASK];
	uint32		num_segments;
	bool		assigned; /* True if a worker claimed this task */
} TpCompactTask;

/*
 * Per-task result - output segment information.
 *
 * After a worker completes a task, it stores the result here for the
 * leader to link into the target level.
 */
typedef struct TpCompactResult
{
	BlockNumber	 output_segment;  /* Root block of merged segment */
	uint32		 target_level;	  /* Level the segment goes to */
	uint64		 docs_merged;	  /* Documents in merged segment */
	uint64		 total_tokens;	  /* Total tokens merged */
	BlockNumber *freed_pages;	  /* Pages to free after linking */
	uint32		 num_freed_pages; /* Count of pages to free */
	bool		 valid;			  /* True if result is valid */
} TpCompactResult;

/*
 * Shared state for parallel compaction.
 *
 * Stored in DSM segment, accessible to all workers and leader.
 */
typedef struct TpParallelCompactShared
{
	/* Immutable configuration (set before workers launch) */
	Oid	   indexrelid;		   /* Index relation OID */
	uint32 worker_count;	   /* Number of workers (including leader) */
	uint32 segments_per_merge; /* From tp_segments_per_level GUC */

	/* Task queue */
	slock_t			 task_mutex; /* Protects task assignment */
	TpCompactTask	 tasks[TP_MAX_COMPACT_WORKERS * TP_MAX_LEVELS];
	uint32			 num_tasks;
	pg_atomic_uint32 next_task; /* Atomic index for task claiming */

	/* Coordination */
	slock_t			  done_mutex;
	ConditionVariable cv;
	pg_atomic_uint32  workers_done;
	pg_atomic_uint32  tasks_completed;

	/* Error handling */
	pg_atomic_uint32 error_occurred; /* Non-zero if any worker errored */
	char			 error_message[256];

	/* Page pool for parallel segment writes */
	int32			 total_pool_pages;
	pg_atomic_uint32 pool_next;

	/*
	 * Variable-length data follows:
	 * - TpCompactResult results[num_tasks]
	 * - BlockNumber page_pool[total_pool_pages]
	 */
} TpParallelCompactShared;

/*
 * Get pointer to results array
 */
static inline TpCompactResult *
TpCompactResults(TpParallelCompactShared *shared)
{
	return (TpCompactResult *)((char *)shared +
							   MAXALIGN(sizeof(TpParallelCompactShared)));
}

/*
 * Get pointer to page pool
 */
static inline BlockNumber *
TpCompactPagePool(TpParallelCompactShared *shared)
{
	char *base = (char *)TpCompactResults(shared);
	base += MAXALIGN(sizeof(TpCompactResult) * shared->num_tasks);
	return (BlockNumber *)base;
}

/*
 * Function declarations
 */

/* Main parallel compaction entry point */
extern void tp_compact_parallel(Relation index);

/* Check if parallel compaction should be used */
extern bool tp_should_compact_parallel(Relation index, uint32 *total_segments);

/* Worker entry point (called by parallel infrastructure) */
extern PGDLLEXPORT void
tp_parallel_compact_worker_main(dsm_segment *seg, shm_toc *toc);
