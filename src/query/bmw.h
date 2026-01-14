/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * bmw.h - Block-Max WAND query optimization
 *
 * Implements early termination for top-k queries by computing upper bounds
 * on block scores and skipping blocks that cannot contribute to results.
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>
#include <utils/memutils.h>

#include "segment/segment.h"
#include "state/state.h"

/*
 * Top-K min-heap for maintaining threshold during scoring.
 *
 * Heap property: parent score <= child scores (minimum at root).
 * This allows O(1) threshold access and O(log k) updates.
 * When heap is full, threshold = root score (minimum in top-k).
 *
 * Supports deferred CTID resolution: segment results store (seg_block, doc_id)
 * and resolve CTIDs only at extraction time. Memtable results have CTIDs
 * immediately (seg_block = InvalidBlockNumber).
 */
typedef struct TpTopKHeap
{
	ItemPointerData *ctids;		/* Array of k CTIDs (resolved at extraction) */
	BlockNumber		*seg_blocks; /* Segment root block (InvalidBlockNumber =
									memtable) */
	uint32			*doc_ids;	 /* Segment-local doc IDs */
	float4			*scores;	 /* Parallel array of scores */
	int				 capacity;	 /* k - maximum results */
	int				 size;		 /* Current entries (0 to k) */
} TpTopKHeap;

/*
 * Initialize a top-k heap.
 * Allocates arrays in the given memory context.
 */
extern void tp_topk_init(TpTopKHeap *heap, int k, MemoryContext ctx);

/*
 * Free a top-k heap's allocated memory.
 */
extern void tp_topk_free(TpTopKHeap *heap);

/*
 * Get current threshold (minimum score to enter top-k).
 * Returns 0 if heap not yet full.
 */
static inline float4
tp_topk_threshold(TpTopKHeap *heap)
{
	return (heap->size >= heap->capacity) ? heap->scores[0] : 0.0f;
}

/*
 * Check if a score is definitely dominated (cannot enter top-k).
 * Quick check to avoid heap operations for non-competitive docs.
 * Returns false for equal scores since they may qualify via CTID tie-breaking.
 */
static inline bool
tp_topk_dominated(TpTopKHeap *heap, float4 score)
{
	return heap->size >= heap->capacity && score < heap->scores[0];
}

/*
 * Add a memtable result to the top-k heap.
 * CTID is known immediately for memtable entries.
 */
extern void
tp_topk_add_memtable(TpTopKHeap *heap, ItemPointerData ctid, float4 score);

/*
 * Add a segment result to the top-k heap.
 * CTID resolution is deferred until extraction.
 */
extern void tp_topk_add_segment(
		TpTopKHeap *heap,
		BlockNumber seg_block,
		uint32		doc_id,
		float4		score);

/*
 * Resolve CTIDs for segment results in the heap.
 * Must be called before tp_topk_extract.
 * Opens segments as needed to look up CTIDs.
 */
extern void tp_topk_resolve_ctids(TpTopKHeap *heap, Relation index);

/*
 * Extract sorted results from heap (descending by score).
 * Returns number of results extracted.
 * After extraction, heap is empty.
 * Note: Call tp_topk_resolve_ctids first if heap contains segment results.
 */
extern int
tp_topk_extract(TpTopKHeap *heap, ItemPointerData *ctids, float4 *scores);

/*
 * BMW statistics for debugging/EXPLAIN ANALYZE
 */
typedef struct TpBMWStats
{
	uint64 blocks_scanned; /* Segment blocks actually scored */
	uint64 blocks_skipped; /* Segment blocks skipped by BMW */
	uint64 memtable_docs;  /* Documents scored from memtable (exhaustive) */
	uint64 segment_docs_scored; /* Documents scored from segments */
	uint64 docs_in_results;		/* Documents in final results */

	/* WAND seek statistics */
	uint64 seeks_performed;	 /* Binary search seeks executed */
	uint64 docs_seeked;		 /* Total doc IDs skipped via seeking */
	uint64 linear_advances;	 /* Single-doc advances (no seek benefit) */
	uint64 seek_to_same_doc; /* Seeks that landed on same/next doc */
} TpBMWStats;

/*
 * Score documents using single-term Block-Max WAND.
 *
 * For single-term queries, uses block-level upper bounds to skip
 * blocks that cannot contribute to top-k results.
 *
 * Returns number of results (up to max_results).
 */
extern int tp_score_single_term_bmw(
		TpLocalIndexState *local_state,
		Relation		   index,
		const char		  *term,
		float4			   idf,
		float4			   k1,
		float4			   b,
		float4			   avg_doc_len,
		int				   max_results,
		ItemPointerData	  *result_ctids,
		float4			  *result_scores,
		TpBMWStats		  *stats);

/*
 * Score documents using multi-term Block-Max WAND.
 *
 * For multi-term queries, uses the WAND algorithm with block-level
 * upper bounds to find top-k documents efficiently.
 *
 * Returns number of results (up to max_results).
 */
extern int tp_score_multi_term_bmw(
		TpLocalIndexState *local_state,
		Relation		   index,
		char			 **terms,
		int				   term_count,
		int32			  *query_freqs,
		float4			  *idfs,
		float4			   k1,
		float4			   b,
		float4			   avg_doc_len,
		int				   max_results,
		ItemPointerData	  *result_ctids,
		float4			  *result_scores,
		TpBMWStats		  *stats);

/*
 * Compute block maximum BM25 score from skip entry metadata.
 *
 * Uses block_max_tf and block_max_norm to compute upper bound on
 * any document's score in the block.
 */
extern float4 tp_compute_block_max_score(
		TpSkipEntry *skip,
		float4		 idf,
		float4		 k1,
		float4		 b,
		float4		 avg_doc_len);
