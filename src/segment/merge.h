/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * merge.h - Segment merge for LSM-style compaction
 */
#pragma once

#include "postgres.h"
#include "segment.h"
#include "segment_io.h"
#include "storage/block.h"
#include "storage/buffile.h"
#include "utils/rel.h"

/* Forward declarations */
struct TpLocalIndexState;
struct TpMergeSource;
struct TpMergedTerm;

/*
 * Merge sink abstraction: unified I/O target for segment writes.
 * Can write to either index pages (via TpSegmentWriter) or BufFile.
 */
typedef struct TpMergeSink
{
	bool   is_buffile;
	uint64 current_offset;

	/* Pages backend (active when !is_buffile) */
	TpSegmentWriter writer;
	Relation		index;

	/* BufFile backend (active when is_buffile) */
	BufFile *file;
	uint64	 base_abs; /* absolute file offset of segment start */
} TpMergeSink;

/* Sink initialization */
extern void merge_sink_init_pages(TpMergeSink *sink, Relation index);
extern void merge_sink_init_pages_parallel(
		TpMergeSink *sink, Relation index, pg_atomic_uint64 *page_counter);
extern void merge_sink_init_buffile(TpMergeSink *sink, BufFile *file);

/*
 * Write a merged segment to sink (pages or BufFile).
 * Unified function that replaces both write_merged_segment() and
 * write_merged_segment_to_buffile().
 */
extern void write_merged_segment_to_sink(
		TpMergeSink			 *sink,
		struct TpMergedTerm	 *terms,
		uint32				  num_terms,
		struct TpMergeSource *sources,
		int					  num_sources,
		uint32				  target_level,
		uint64				  total_tokens,
		bool				  disjoint_sources);

/*
 * Merge all segments at the specified level into a single segment
 * at level+1.
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
 *   The root block of the new merged segment, or InvalidBlockNumber
 * on failure.
 *
 * Note: The caller is responsible for holding an appropriate lock on
 * the index relation. This function modifies the metapage to update
 * level chains.
 */
extern BlockNumber
tp_merge_level_segments(Relation index, uint32 level, uint32 max_merge);

/*
 * Check if a level needs compaction and trigger merge if so.
 *
 * Called after adding a segment to check if the level has reached
 * tp_segments_per_level. If so, merges up to segments_per_level
 * segments per batch, then recursively checks the next level.
 *
 * Parameters:
 *   index - The index relation (must be opened with appropriate lock)
 *   level - The level to check (0 = L0, 1 = L1, etc.)
 */
extern void tp_maybe_compact_level(Relation index, uint32 level);

/*
 * Compact all segments across all levels into one segment per level.
 *
 * Unlike tp_maybe_compact_level, this ignores the segments_per_level
 * threshold and merges ALL segments at each level in one batch.
 * Used after parallel index build to produce a fully compacted index.
 *
 * Parameters:
 *   index - The index relation (must be opened with appropriate lock)
 */
extern void tp_force_merge_all(Relation index);
