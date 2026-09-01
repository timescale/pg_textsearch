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
