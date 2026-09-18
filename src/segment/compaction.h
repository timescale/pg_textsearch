/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

#include "index/metapage.h"

struct TpLocalIndexState;
typedef struct RelationData *Relation;

typedef enum TpCompactionAllocationPause
{
	TP_COMPACTION_ALLOCATION_PAUSE_NONE,
	TP_COMPACTION_ALLOCATION_PAUSE_OUTPUT_DATA,
	TP_COMPACTION_ALLOCATION_PAUSE_PAGE_INDEX,
	TP_COMPACTION_ALLOCATION_PAUSE_TOMBSTONE
} TpCompactionAllocationPause;

extern int tp_debug_compaction_pause_after_select_ms;
extern int tp_debug_compaction_pause_source_estimate_ms;
extern int tp_debug_compaction_pause_before_publish_ms;
extern int tp_debug_compaction_pause_after_restamp_ms;
extern int tp_debug_compaction_pause_after_allocation;

extern void tp_debug_compaction_allocation_pause(
		Relation index, TpCompactionAllocationPause phase);

extern uint64 tp_max_segment_size_bytes(void);

extern void tp_compaction_lock(Relation index);
extern void tp_compaction_unlock(Relation index);

/*
 * Report whether any level holds at least segments_per_level segments.
 * Advisory only; see the comment on the definition.
 */
extern bool tp_compaction_needed(Relation index);

/*
 * Run at most one bounded compaction pass and report whether one ran.
 * Runtime callers hold the per-index maintenance object lock. CREATE INDEX may
 * instead call while holding its private per-index exclusive lock.
 */
extern bool
tp_compact_step(struct TpLocalIndexState *index_state, Relation index);

/*
 * Run at most one below-threshold pass whose selected prefix contains an
 * already-empty V5 segment.  The caller already holds maintenance.
 */
extern bool
tp_compact_empty_step(struct TpLocalIndexState *index_state, Relation index);

/*
 * Publish one already-built replacement for one published source segment.
 * The caller holds maintenance and assigns reclaim_fxid before calling.
 * This function takes ownership of replacement_root on entry.
 */
extern void tp_publish_prepared_segment_replacement(
		struct TpLocalIndexState *index_state,
		Relation				  index,
		uint32					  level,
		BlockNumber				  source_root,
		BlockNumber				  replacement_root,
		uint64					  removed_docs,
		uint64					  removed_tokens,
		FullTransactionId		  reclaim_fxid);

extern void
tp_force_compact(struct TpLocalIndexState *index_state, Relation index);
