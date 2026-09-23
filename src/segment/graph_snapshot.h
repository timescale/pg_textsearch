/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * graph_snapshot.h - Atomic segment-root and memtable read snapshots
 */
#pragma once

#include <postgres.h>

#include <storage/block.h>
#include <utils/rel.h>

#include "index/metapage.h"
#include "memtable/chain_walker.h"

/*
 * A capture of the published graph: the metapage, the memtable chain
 * endpoint, and the per-level segment roots.
 *
 * Lifetime contract: a snapshot owns no buffer locks and no pins once
 * created, only copied block numbers.  Traversing them afterwards is
 * safe only because displaced segment pages are parked in the
 * deferred-free tombstone chain (issue #380) until the reclaim horizon
 * passes.  A caller must therefore either hold the per-index lock or
 * run under a transaction snapshot that holds that horizon back, which
 * on a hot standby additionally requires hot_standby_feedback = on.
 *
 * roots_captured distinguishes a full capture from a metadata-only one;
 * without it an uncaptured graph is indistinguishable from an index
 * that genuinely has no segments.
 */
typedef struct TpSegmentGraphSnapshot
{
	TpIndexMetaPageData		metapage;
	TpMemtableChainSnapshot memtable;
	BlockNumber			   *roots;
	uint32					level_offsets[TP_MAX_LEVELS + 1];
	uint32					root_count;
	bool					roots_captured;
} TpSegmentGraphSnapshot;

extern int tp_debug_segment_graph_snapshot_pause_before_lock_ms;
extern int tp_debug_segment_graph_snapshot_pause_before_unlock_ms;
extern int tp_debug_segment_graph_snapshot_pause_ms;

extern TpSegmentGraphSnapshot *
tp_segment_graph_snapshot_create(Relation index);
extern TpSegmentGraphSnapshot *
tp_segment_graph_snapshot_create_metadata(Relation index);
extern const BlockNumber *tp_segment_graph_snapshot_level(
		const TpSegmentGraphSnapshot *snapshot, uint32 level, uint32 *count);
extern void tp_segment_graph_snapshot_free(TpSegmentGraphSnapshot *snapshot);
