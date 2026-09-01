/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * merge.h - Segment merge for LSM-style compaction
 */
#pragma once

#include "postgres.h"
#include "segment/io.h"
#include "segment/segment.h"
#include "storage/block.h"
#include "utils/rel.h"

/* Forward declarations */
struct TpLocalIndexState;
struct TpMergeSource;
struct TpMergedTerm;

typedef struct TpMergedSegmentResult
{
	BlockNumber root;
	uint32		num_pages;
	uint32		num_docs;
	uint64		total_tokens;
	uint64		data_size;
} TpMergedSegmentResult;

/*
 * Merge sink: writes merged segment data to index pages.
 */
typedef struct TpMergeSink
{
	uint64			current_offset;
	TpSegmentWriter writer;
	Relation		index;
} TpMergeSink;

/* Sink initialization */
extern void merge_sink_init_pages(TpMergeSink *sink, Relation index);

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
		bool				  disjoint_sources,
		BlockNumber			  next_segment);

/*
 * Merge exactly the supplied immutable segments into one unpublished
 * current-format segment.  The caller publishes or discards the result.
 */
extern bool tp_merge_segment_batch(
		Relation			   index,
		const BlockNumber	  *source_roots,
		uint32				   num_sources,
		uint32				   output_level,
		BlockNumber			   next_segment,
		TpMergedSegmentResult *result);

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
 *   The root block of the new merged segment, or InvalidBlockNumber when
 *   every source document is dead.
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
