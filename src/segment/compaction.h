/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

#include "index/metapage.h"

struct TpLocalIndexState;
typedef struct RelationData *Relation;

typedef enum TpCompactionAllocationPhase
{
	TP_COMPACTION_ALLOCATION_OUTPUT_DATA,
	TP_COMPACTION_ALLOCATION_PAGE_INDEX,
	TP_COMPACTION_ALLOCATION_TOMBSTONE
} TpCompactionAllocationPhase;

typedef enum TpSegmentReplacementReclaimMode
{
	TP_SEGMENT_REPLACEMENT_RECLAIM_CURRENT_XID,
	TP_SEGMENT_REPLACEMENT_RECLAIM_NEXT_XID
} TpSegmentReplacementReclaimMode;

extern void
tp_compaction_allocation_injection_point(TpCompactionAllocationPhase phase);

extern uint64 tp_max_segment_size_bytes(void);

extern void tp_compaction_lock(Relation index);
extern bool tp_try_compaction_lock(Relation index);
extern void tp_compaction_unlock(Relation index);
extern void tp_compaction_publication_lock(Relation index, LOCKMODE mode);
extern bool tp_try_compaction_publication_lock(Relation index, LOCKMODE mode);
extern void tp_compaction_publication_unlock(Relation index, LOCKMODE mode);

/*
 * Report whether any level holds at least segments_per_level segments.
 * A cheap count-only gate; see the comment on the definition.
 */
extern bool tp_compaction_needed(Relation index);

/*
 * Report whether tp_compact_step would run a pass, accounting for
 * levels whose segments are all over max_segment_size.  Safe as a loop
 * condition.
 */
extern bool tp_compaction_pass_available(
		struct TpLocalIndexState *index_state, Relation index);

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
 * The caller holds maintenance and passes ownership of replacement_root.
 * Serial VACUUM uses its current transaction ID; parallel VACUUM attaches a
 * provisional tombstone batch, then samples and WAL-restamps it after the
 * publication record is inserted.
 */
extern void tp_publish_prepared_segment_replacement(
		struct TpLocalIndexState	   *index_state,
		Relation						index,
		uint32							level,
		BlockNumber						source_root,
		BlockNumber						replacement_root,
		uint64							removed_docs,
		uint64							removed_tokens,
		uint64							added_tokens,
		TpSegmentReplacementReclaimMode reclaim_mode);

extern void
tp_force_compact(struct TpLocalIndexState *index_state, Relation index);
