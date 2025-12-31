/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * segment_merge.h - Segment merge for LSM-style compaction
 */
#pragma once

#include "postgres.h"
#include "storage/block.h"
#include "utils/rel.h"

/* Forward declarations */
struct TpLocalIndexState;

/*
 * Merge all segments at the specified level into a single segment at level+1.
 *
 * This performs an N-way merge of all segments in the level's chain:
 * 1. Opens all segment readers for the level
 * 2. Merges term dictionaries using linear scan (O(n) per term)
 * 3. Combines posting lists for duplicate terms
 * 4. Writes merged segment at the next level
 * 5. Updates metapage to reflect the new structure
 *
 * Parameters:
 *   index - The index relation (must be opened with appropriate lock)
 *   level - The level to merge (0 = L0, 1 = L1, etc.)
 *
 * Returns:
 *   The root block of the new merged segment, or InvalidBlockNumber on
 * failure.
 *
 * Note: The caller is responsible for holding an appropriate lock on the
 * index relation. This function modifies the metapage to update level chains.
 */
extern BlockNumber tp_merge_level_segments(Relation index, uint32 level);

/*
 * Check if a level needs compaction and trigger merge if so.
 *
 * Called after adding a segment to check if the level has reached
 * tp_segments_per_level. If so, merges all segments at that level
 * into one segment at the next level, then recursively checks the
 * next level.
 *
 * Parameters:
 *   index - The index relation (must be opened with appropriate lock)
 *   level - The level to check (0 = L0, 1 = L1, etc.)
 */
extern void tp_maybe_compact_level(Relation index, uint32 level);
