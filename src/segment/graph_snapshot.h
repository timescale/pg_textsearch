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

typedef struct TpSegmentGraphSnapshot
{
	TpIndexMetaPageData		metapage;
	TpMemtableChainSnapshot memtable;
	BlockNumber			   *roots;
	uint32					level_offsets[TP_MAX_LEVELS + 1];
	uint32					root_count;
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
