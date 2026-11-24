/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * bmw.c - Block-Max WAND query optimization implementation
 */
#include <postgres.h>

#include <float.h>
#include <math.h>
#include <miscadmin.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>

#include "constants.h"
#include "index/metapage.h"
#include "index/source.h"
#include "memtable/chain_source.h"
#include "scoring/bm25.h"
#include "scoring/bmw.h"
#include "segment/alive_bitset.h"
#include "segment/compression.h"
#include "segment/fieldnorm.h"
#include "segment/io.h"

/*
 * ------------------------------------------------------------
 * Top-K Min-Heap Implementation
 * ------------------------------------------------------------
 */

void
tp_topk_init(TpTopKHeap *heap, int k, MemoryContext ctx)
{
	MemoryContext old_ctx = MemoryContextSwitchTo(ctx);

	heap->ctids		 = palloc(k * sizeof(ItemPointerData));
	heap->seg_blocks = palloc(k * sizeof(BlockNumber));
	heap->doc_ids	 = palloc(k * sizeof(uint32));
	heap->scores	 = palloc(k * sizeof(float4));
	heap->capacity	 = k;
	heap->size		 = 0;

	MemoryContextSwitchTo(old_ctx);
}

void
tp_topk_free(TpTopKHeap *heap)
{
	if (heap->ctids)
	{
		pfree(heap->ctids);
		heap->ctids = NULL;
	}
	if (heap->seg_blocks)
	{
		pfree(heap->seg_blocks);
		heap->seg_blocks = NULL;
	}
	if (heap->doc_ids)
	{
		pfree(heap->doc_ids);
		heap->doc_ids = NULL;
	}
	if (heap->scores)
	{
		pfree(heap->scores);
		heap->scores = NULL;
	}
	heap->capacity = 0;
	heap->size	   = 0;
}

/*
 * Swap two heap entries.
 */
static inline void
heap_swap(TpTopKHeap *heap, int i, int j)
{
	ItemPointerData tmp_ctid	  = heap->ctids[i];
	BlockNumber		tmp_seg_block = heap->seg_blocks[i];
	uint32			tmp_doc_id	  = heap->doc_ids[i];
	float4			tmp_score	  = heap->scores[i];

	heap->ctids[i]		= heap->ctids[j];
	heap->seg_blocks[i] = heap->seg_blocks[j];
	heap->doc_ids[i]	= heap->doc_ids[j];
	heap->scores[i]		= heap->scores[j];

	heap->ctids[j]		= tmp_ctid;
	heap->seg_blocks[j] = tmp_seg_block;
	heap->doc_ids[j]	= tmp_doc_id;
	heap->scores[j]		= tmp_score;
}

/*
 * Compare two heap entries for min-heap ordering.
 * Returns true if entry at position a should be closer to the root
 * (i.e., is a weaker result that should be evicted first).
 *
 * Primary: lower score = weaker = closer to root.
 * Secondary: among equal scores, higher CTID = weaker (lower CTID wins
 * ties in the final output). For segment entries whose CTIDs are not
 * yet resolved, doc_id within the segment is a reliable proxy.
 */
static inline bool
heap_less(TpTopKHeap *heap, int a, int b)
{
	if (heap->scores[a] < heap->scores[b])
		return true;
	if (heap->scores[a] > heap->scores[b])
		return false;

	/* Equal scores: higher CTID is weaker (closer to root) */
	if (ItemPointerIsValid(&heap->ctids[a]) &&
		ItemPointerIsValid(&heap->ctids[b]))
		return ItemPointerCompare(&heap->ctids[a], &heap->ctids[b]) > 0;

	/* Segment entries: doc_id proxy (higher doc_id = weaker) */
	if (heap->doc_ids[a] != heap->doc_ids[b])
		return heap->doc_ids[a] > heap->doc_ids[b];
	return heap->seg_blocks[a] > heap->seg_blocks[b];
}

/*
 * Sift up: restore heap property after insertion at position i.
 * For min-heap: parent <= children (using heap_less for comparison).
 */
static void
heap_sift_up(TpTopKHeap *heap, int i)
{
	while (i > 0)
	{
		int parent = (i - 1) / 2;
		if (!heap_less(heap, i, parent))
			break;
		heap_swap(heap, i, parent);
		i = parent;
	}
}

/*
 * Sift down: restore heap property after replacement at position i.
 * For min-heap: parent <= children (using heap_less for comparison).
 */
static void
heap_sift_down(TpTopKHeap *heap, int i)
{
	while (true)
	{
		int left	 = 2 * i + 1;
		int right	 = 2 * i + 2;
		int smallest = i;

		if (left < heap->size && heap_less(heap, left, smallest))
			smallest = left;
		if (right < heap->size && heap_less(heap, right, smallest))
			smallest = right;

		if (smallest == i)
			break;

		heap_swap(heap, i, smallest);
		i = smallest;
	}
}

/*
 * Check if a candidate entry should replace the heap root.
 *
 * Higher score always wins. For equal scores, lower CTID wins
 * (matching the extraction sort order). When CTIDs are not both
 * available (memtable vs segment), doc_id serves as proxy.
 */
static inline bool
heap_beats_root(
		TpTopKHeap *heap, float4 score, ItemPointerData *ctid, uint32 doc_id)
{
	if (score > heap->scores[0])
		return true;
	if (score < heap->scores[0])
		return false;

	/* Equal score: lower CTID beats higher CTID */
	if (ctid && ItemPointerIsValid(ctid) &&
		ItemPointerIsValid(&heap->ctids[0]))
		return ItemPointerCompare(ctid, &heap->ctids[0]) < 0;

	/* Both segment entries: lower doc_id proxy */
	if (ctid == NULL && !ItemPointerIsValid(&heap->ctids[0]))
		return doc_id < heap->doc_ids[0];

	/* Mixed memtable/segment: can't compare, don't evict */
	return false;
}

/*
 * Add a memtable result to the top-k heap.
 * CTID is known immediately for memtable entries.
 */
void
tp_topk_add_memtable(TpTopKHeap *heap, ItemPointerData ctid, float4 score)
{
	if (heap->size < heap->capacity)
	{
		/* Heap not full - just add */
		int i				= heap->size++;
		heap->ctids[i]		= ctid;
		heap->seg_blocks[i] = InvalidBlockNumber; /* Marks as memtable entry */
		heap->doc_ids[i]	= 0;
		heap->scores[i]		= score;
		heap_sift_up(heap, i);
	}
	else if (heap_beats_root(heap, score, &ctid, 0))
	{
		/* Heap full but new entry beats root */
		heap->ctids[0]		= ctid;
		heap->seg_blocks[0] = InvalidBlockNumber;
		heap->doc_ids[0]	= 0;
		heap->scores[0]		= score;
		heap_sift_down(heap, 0);
	}
	/* else: doesn't qualify for top-k, ignore */
}

/*
 * Add a segment result to the top-k heap.
 * CTID resolution is deferred until extraction.
 */
void
tp_topk_add_segment(
		TpTopKHeap *heap, BlockNumber seg_block, uint32 doc_id, float4 score)
{
	if (heap->size < heap->capacity)
	{
		/* Heap not full - just add */
		int i = heap->size++;
		ItemPointerSetInvalid(&heap->ctids[i]); /* Will be resolved later */
		heap->seg_blocks[i] = seg_block;
		heap->doc_ids[i]	= doc_id;
		heap->scores[i]		= score;
		heap_sift_up(heap, i);
	}
	else if (heap_beats_root(heap, score, NULL, doc_id))
	{
		/* Heap full but new entry beats root */
		ItemPointerSetInvalid(&heap->ctids[0]);
		heap->seg_blocks[0] = seg_block;
		heap->doc_ids[0]	= doc_id;
		heap->scores[0]		= score;
		heap_sift_down(heap, 0);
	}
	/* else: doesn't qualify for top-k, ignore */
}

/*
 * Resolve CTIDs for segment results in the heap.
 * Batches lookups by segment - opens each unique segment once, resolves all
 * CTIDs from that segment, then closes it. This avoids O(k) segment opens.
 */
void
tp_topk_resolve_ctids(TpTopKHeap *heap, Relation index)
{
	int i, j;

	for (i = 0; i < heap->size; i++)
	{
		TpSegmentReader *reader;
		BlockNumber		 seg_block;

		/* Skip memtable entries and already-resolved entries */
		if (heap->seg_blocks[i] == InvalidBlockNumber)
			continue;

		seg_block = heap->seg_blocks[i];

		/* Open segment once for all entries from this segment */
		reader = tp_segment_open_ex(index, seg_block, false);
		if (reader == NULL)
			continue;

		/* Resolve all CTIDs from this segment */
		for (j = i; j < heap->size; j++)
		{
			if (heap->seg_blocks[j] == seg_block)
			{
				tp_segment_lookup_ctid(
						reader, heap->doc_ids[j], &heap->ctids[j]);
				/* Mark as resolved by setting seg_block to invalid */
				heap->seg_blocks[j] = InvalidBlockNumber;
			}
		}

		tp_segment_close(reader);
	}
}

/*
 * Compare function for qsort: sort by (score DESC, CTID ASC).
 * This matches the exhaustive path's tie-breaking for deterministic results.
 */
static int
compare_heap_entries(const void *a, const void *b, void *arg)
{
	int			i	 = *(const int *)a;
	int			j	 = *(const int *)b;
	TpTopKHeap *heap = (TpTopKHeap *)arg;
	int			cmp;

	/* Primary: higher score first (descending) */
	if (heap->scores[i] > heap->scores[j])
		return -1;
	if (heap->scores[i] < heap->scores[j])
		return 1;

	/* Secondary: lower CTID first (ascending) for deterministic tie-breaking
	 */
	cmp = ItemPointerCompare(&heap->ctids[i], &heap->ctids[j]);
	return cmp;
}

/*
 * Extract sorted results (descending by score, CTID tie-breaking).
 *
 * Note: Call tp_topk_resolve_ctids first if heap contains segment results.
 * This ensures CTIDs are available for tie-breaking to match exhaustive path.
 */
int
tp_topk_extract(TpTopKHeap *heap, ItemPointerData *ctids, float4 *scores)
{
	int	 count = heap->size;
	int *indices;
	int	 i;

	if (count == 0)
		return 0;

	/* Create index array for sorting */
	indices = palloc(count * sizeof(int));
	for (i = 0; i < count; i++)
		indices[i] = i;

	/* Sort indices by (score DESC, CTID ASC) */
	qsort_arg(indices, count, sizeof(int), compare_heap_entries, heap);

	/* Copy to output in sorted order */
	for (i = 0; i < count; i++)
	{
		int idx	  = indices[i];
		ctids[i]  = heap->ctids[idx];
		scores[i] = heap->scores[idx];
	}

	pfree(indices);
	heap->size = 0;

	return count;
}

/*
 * ------------------------------------------------------------
 * Block Max Score Computation
 * ------------------------------------------------------------
 */

float4
tp_compute_block_max_score(
		TpSkipEntry *skip, float4 idf, float4 k1, float4 b, float4 avg_doc_len)
{
	float4 tf = (float4)skip->block_max_tf;
	float4 dl = (float4)decode_fieldnorm(skip->block_max_norm);

	/* BM25 formula with max TF and min doc length in block */
	float4 len_norm		= 1.0f - b + b * (dl / avg_doc_len);
	float4 tf_component = (tf * (k1 + 1.0f)) / (tf + k1 * len_norm);

	return idf * tf_component;
}

/*
 * Compute BM25 score for a single posting.
 */
static inline float4
compute_bm25_score(
		float4 idf,
		int32  tf,
		int32  doc_len,
		float4 k1,
		float4 b,
		float4 avg_doc_len)
{
	float4 len_norm		= 1.0f - b + b * ((float4)doc_len / avg_doc_len);
	float4 tf_component = ((float4)tf * (k1 + 1.0f)) /
						  ((float4)tf + k1 * len_norm);

	return idf * tf_component;
}

/*
 * ------------------------------------------------------------
 * Single-Term BMW Scoring
 * ------------------------------------------------------------
 */

/*
 * Score memtable postings for a single term.
 * Memtable has no skip index, so we score all postings exhaustively.
 *
 * Caller owns `source` (a TpMemtableChainSource over the index's
 * on-disk chain) and is responsible for closing it.  If `source`
 * is NULL we skip memtable scoring entirely.
 */
static void
score_memtable_single_term(
		TpTopKHeap	 *heap,
		TpDataSource *source,
		const char	 *term,
		float4		  idf,
		float4		  k1,
		float4		  b,
		float4		  avg_doc_len,
		TpBMWStats	 *stats,
		TpBMWFilter	 *filter)
{
	TpPostingData *postings;
	int			   i;

	if (!source)
		return;

	/* Get postings for this term */
	postings = tp_source_get_postings(source, term);
	if (!postings || postings->count == 0)
	{
		if (postings)
			tp_source_free_postings(source, postings);
		return;
	}

	/* Score each posting */
	for (i = 0; i < postings->count; i++)
	{
		ItemPointerData *ctid = &postings->ctids[i];
		int32			 tf	  = postings->frequencies[i];
		int32			 doc_len;
		float4			 score;

		if ((i & 0xFFF) == 0)
			CHECK_FOR_INTERRUPTS();

		/* Get document length */
		doc_len = tp_source_get_doc_length(source, ctid);
		if (doc_len <= 0)
			doc_len = 1; /* Fallback for missing entries */

		/* Quantize like segments so scores do not change on spill */
		doc_len = (int32)decode_fieldnorm(encode_fieldnorm((uint32)doc_len));

		score = compute_bm25_score(idf, tf, doc_len, k1, b, avg_doc_len);

		if (!tp_topk_dominated(heap, score) &&
			(filter == NULL ||
			 filter->memtable_matches(filter->state, source, ctid)))
			tp_topk_add_memtable(heap, *ctid, score);

		if (stats)
			stats->memtable_docs++;
	}

	tp_source_free_postings(source, postings);
}

/*
 * Score segment postings for a single term using BMW.
 */
static void
score_segment_single_term_bmw(
		TpTopKHeap		*heap,
		TpSegmentReader *reader,
		const char		*term,
		float4			 idf,
		float4			 k1,
		float4			 b,
		float4			 avg_doc_len,
		TpBMWStats		*stats,
		TpBMWFilter		*filter)
{
	TpSegmentPostingIterator iter;
	TpSegmentPosting		*posting;
	TpDictEntry				*dict_entry;
	uint32					 block_count;
	float4					*block_max_scores;
	uint32					*block_last_doc_ids = NULL;
	uint32					 i;
	bool drive_candidates = filter != NULL && filter->segment_next != NULL;

	/* Initialize iterator for this term */
	if (!tp_segment_posting_iterator_init(&iter, reader, term))
		return; /* Term not found in segment */
	iter.force_copy = filter != NULL;

	if (drive_candidates)
	{
		uint32 candidate;
		drive_candidates =
				filter->segment_next(filter->state, reader, 0, &candidate);
		if (drive_candidates && candidate == UINT32_MAX)
		{
			tp_segment_posting_iterator_free(&iter);
			return;
		}
	}

	/* Get dictionary entry for block count and skip index */
	dict_entry	= &iter.dict_entry;
	block_count = dict_entry->block_count;

	/* Pre-compute block max scores */
	block_max_scores = palloc(block_count * sizeof(float4));
	if (drive_candidates)
		block_last_doc_ids = palloc(block_count * sizeof(uint32));
	for (i = 0; i < block_count; i++)
	{
		TpSkipEntry skip;
		tp_segment_read_skip_entry(
				reader, dict_entry->skip_index_offset, i, &skip);
		block_max_scores[i] =
				tp_compute_block_max_score(&skip, idf, k1, b, avg_doc_len);
		if (block_last_doc_ids != NULL)
			block_last_doc_ids[i] = skip.last_doc_id;
	}

	/* Process blocks with BMW */
	for (i = 0; i < block_count; i++)
	{
		float4 threshold = tp_topk_threshold(heap);
		float4 block_max = block_max_scores[i];

		CHECK_FOR_INTERRUPTS();

		/* Skip block if it can't beat threshold */
		if (block_max < threshold)
		{
			if (stats)
				stats->blocks_skipped++;
			continue;
		}

		if (stats)
			stats->blocks_scanned++;

		/* Load and score this block */
		iter.current_block = i;
		iter.finished	   = false; /* Reset so we can process this block */
		tp_segment_posting_iterator_load_block(&iter);

		while (tp_segment_posting_iterator_next(&iter, &posting))
		{
			float4 score;

			/*
			 * Break if iterator auto-advanced to next block.
			 * This ensures we only process block i, allowing the outer
			 * for loop to apply threshold checks to subsequent blocks.
			 */
			if (iter.current_block != i)
				break;

			while (drive_candidates)
			{
				uint32 candidate;
				CHECK_FOR_INTERRUPTS();
				if (!filter->segment_next(
							filter->state,
							reader,
							posting->doc_id,
							&candidate))
				{
					drive_candidates = false;
					break;
				}
				if (candidate == UINT32_MAX)
					goto done;
				if (candidate <= posting->doc_id)
					break;
				if (candidate <= iter.skip_entry.last_doc_id)
				{
					do
					{
						if (!tp_segment_posting_iterator_next(&iter, &posting))
							goto done;
					} while (posting->doc_id < candidate);
					continue;
				}
				{
					uint32 target_block = i + 1;
					uint32 end			= block_count;

					while (target_block < end)
					{
						uint32 mid = target_block + (end - target_block) / 2;
						if (block_last_doc_ids[mid] < candidate)
							target_block = mid + 1;
						else
							end = mid;
					}
					if (stats)
						stats->blocks_skipped += target_block - i - 1;
					if (target_block == block_count)
						goto done;
					/* Apply the BMW gate before loading the target block. */
					if (block_max_scores[target_block] <
						tp_topk_threshold(heap))
					{
						i = target_block - 1;
						goto next_block;
					}
				}
				if (!tp_segment_posting_iterator_seek(
							&iter, candidate, &posting))
					goto done;
				/* Consume it unless seek's next-block fallback already did. */
				if (iter.current_in_block < iter.skip_entry.doc_count &&
					iter.block_postings[iter.current_in_block].doc_id ==
							posting->doc_id)
					iter.current_in_block++;
				i = iter.current_block;
				if (stats)
				{
					stats->blocks_scanned++;
					stats->seeks_performed++;
				}
			}

			/* Skip dead docs */
			if (!tp_segment_is_alive(reader, posting->doc_id))
			{
				if (stats)
					stats->dead_docs_skipped++;
				continue;
			}

			score = compute_bm25_score(
					idf,
					posting->frequency,
					posting->doc_length,
					k1,
					b,
					avg_doc_len);

			if (!tp_topk_dominated(heap, score) &&
				(filter == NULL ||
				 filter->segment_matches(
						 filter->state, reader, posting->doc_id)))
			{
				tp_topk_add_segment(
						heap, reader->root_block, posting->doc_id, score);
			}

			if (stats)
				stats->segment_docs_scored++;
		}
	next_block:;
	}

done:
	if (block_last_doc_ids != NULL)
		pfree(block_last_doc_ids);
	pfree(block_max_scores);
	tp_segment_posting_iterator_free(&iter);
}

int
tp_score_single_term_bmw(
		TpLocalIndexState			 *local_state,
		Relation					  index,
		const TpSegmentGraphSnapshot *snapshot,
		TpDataSource				 *memtable_src,
		const char					 *term,
		float4						  idf,
		float4						  k1,
		float4						  b,
		float4						  avg_doc_len,
		int							  max_results,
		ItemPointerData				 *result_ctids,
		float4						 *result_scores,
		TpBMWStats					 *stats,
		TpBMWFilter					 *filter)
{
	TpTopKHeap heap;
	int		   result_count;

	(void)local_state; /* reserved for future use */

	/* Initialize stats */
	if (stats)
		memset(stats, 0, sizeof(TpBMWStats));

	/* Initialize top-k heap */
	tp_topk_init(&heap, max_results, CurrentMemoryContext);

	/* Score memtable (exhaustive - no skip index) */
	score_memtable_single_term(
			&heap, memtable_src, term, idf, k1, b, avg_doc_len, stats, filter);
	if (filter != NULL)
		filter->reset(filter->state);

	/* Score each segment level with BMW */
	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		uint32			   root_count;
		const BlockNumber *roots =
				tp_segment_graph_snapshot_level(snapshot, level, &root_count);

		for (uint32 root_idx = 0; root_idx < root_count; root_idx++)
		{
			TpSegmentReader *reader = tp_segment_open(index, roots[root_idx]);

			CHECK_FOR_INTERRUPTS();

			score_segment_single_term_bmw(
					&heap,
					reader,
					term,
					idf,
					k1,
					b,
					avg_doc_len,
					stats,
					filter);
			if (filter != NULL)
				filter->reset(filter->state);

			tp_segment_close(reader);
		}
	}

	/* Resolve CTIDs for segment results before extraction */
	tp_topk_resolve_ctids(&heap, index);

	/* Extract sorted results */
	result_count = tp_topk_extract(&heap, result_ctids, result_scores);

	if (stats)
		stats->docs_in_results = result_count;

	return result_count;
}

/*
 * ------------------------------------------------------------
 * Multi-Term BMW Scoring
 * ------------------------------------------------------------
 */

/*
 * Per-term state for multi-term BMW scoring.
 */
typedef struct TpTermState
{
	const char *term;
	float4		idf;
	int32		query_freq; /* Query term frequency (for boosting) */
	bool		required;	/* Required by the Boolean predicate */

	/* Global maximum score across all blocks (for WAND pivot) */
	float4 max_score;

	/* Segment-specific state (reset per segment) */
	bool					 found; /* Term found in current segment */
	TpSegmentPostingIterator iter;	/* Iterator (contains dict_entry) */
	float4 *block_max_scores;		/* Pre-computed block max scores */
	uint32 *block_last_doc_ids;		/* Cached last_doc_id per block */
	uint32	cur_doc_id; /* Posting ID, or a pending seek lower bound */
	uint32	bound_block;
	bool	seek_pending;
} TpTermState;

/*
 * Refresh cached doc ID from iterator state.
 */
static inline void
refresh_cur_doc_id(TpTermState *ts)
{
	if (!ts->found || ts->iter.finished)
		ts->cur_doc_id = UINT32_MAX;
	else
		ts->cur_doc_id = tp_segment_posting_iterator_current_doc_id(&ts->iter);
}

/*
 * Get current doc ID for a term, or UINT32_MAX if exhausted.
 * Uses cached value maintained by advance/seek/init functions.
 */
static inline uint32
term_current_doc_id(TpTermState *ts)
{
	return ts->cur_doc_id;
}

/*
 * Score memtable postings for multiple terms.
 * Memtable has no skip index, so we score all postings exhaustively.
 */
static void
score_memtable_multi_term(
		TpTopKHeap	 *heap,
		TpDataSource *source,
		TpTermState **terms,
		int			  term_count,
		float4		  k1,
		float4		  b,
		float4		  avg_doc_len,
		TpBMWStats	 *stats,
		TpBMWFilter	 *filter)
{
	HTAB   *doc_accum;
	HASHCTL hash_ctl;
	int		term_idx;

	if (!source)
		return;

	/* Create hash table for document score accumulation */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(DocumentScoreEntry);
	hash_ctl.hcxt	   = CurrentMemoryContext;
	doc_accum		   = hash_create(
			 "Memtable Doc Accum", 1024, &hash_ctl, HASH_ELEM | HASH_BLOBS);

	/* Process each term */
	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState	  *ts = terms[term_idx];
		TpPostingData *postings;
		int			   i;

		postings = tp_source_get_postings(source, ts->term);
		if (!postings || postings->count == 0)
		{
			if (postings)
				tp_source_free_postings(source, postings);
			continue;
		}

		/* Score each posting for this term */
		for (i = 0; i < postings->count; i++)
		{
			ItemPointerData	   *ctid = &postings->ctids[i];
			int32				tf	 = postings->frequencies[i];
			int32				doc_len;
			float4				term_score;
			DocumentScoreEntry *entry;
			bool				found;

			/* CHECK every 4096 postings; this loop can be very long
			 * when the memtable holds millions of postings -- gating
			 * amortizes the overhead. */
			if ((i & 0xFFF) == 0)
				CHECK_FOR_INTERRUPTS();

			/* Get document length */
			doc_len = tp_source_get_doc_length(source, ctid);
			if (doc_len <= 0)
				doc_len = 1;

			/* Quantize like segments so scores do not change on spill */
			doc_len = (int32)decode_fieldnorm(
					encode_fieldnorm((uint32)doc_len));

			/* Compute BM25 term contribution */
			term_score = compute_bm25_score(
								 ts->idf, tf, doc_len, k1, b, avg_doc_len) *
						 ts->query_freq;

			/* Accumulate in hash table */
			entry = (DocumentScoreEntry *)
					hash_search(doc_accum, ctid, HASH_ENTER, &found);

			if (!found)
			{
				entry->ctid		  = *ctid;
				entry->score	  = term_score;
				entry->doc_length = (float4)doc_len;
			}
			else
			{
				entry->score += term_score;
			}
		}

		tp_source_free_postings(source, postings);
	}

	/* Add accumulated documents to heap */
	{
		HASH_SEQ_STATUS		seq;
		DocumentScoreEntry *entry;

		hash_seq_init(&seq, doc_accum);
		while ((entry = hash_seq_search(&seq)) != NULL)
		{
			if (!tp_topk_dominated(heap, entry->score) &&
				(filter == NULL ||
				 filter->memtable_matches(
						 filter->state, source, &entry->ctid)))
				tp_topk_add_memtable(heap, entry->ctid, entry->score);

			if (stats)
				stats->memtable_docs++;
		}
	}

	hash_destroy(doc_accum);
}

/*
 * Advance a term iterator to the next document.
 * Returns true if iterator is still active, false if exhausted.
 */
static bool
advance_term_iterator(TpTermState *ts)
{
	Assert(!ts->seek_pending);
	ts->iter.current_in_block++;

	if (ts->iter.current_in_block >= ts->iter.skip_entry.doc_count)
	{
		ts->iter.current_block++;
		if (ts->iter.current_block >= ts->iter.dict_entry.block_count)
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
		ts->iter.current_in_block = 0;
		if (!tp_segment_posting_iterator_load_block(&ts->iter))
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
	}
	ts->cur_doc_id = ts->iter.block_postings[ts->iter.current_in_block].doc_id;
	return true;
}

/*
 * Seek a term iterator to target doc ID using binary search on cached skip
 * data. Returns true if iterator is still active (positioned at doc >=
 * target), false if exhausted.
 *
 * Uses pre-loaded block_last_doc_ids for O(log blocks) in-memory binary
 * search, avoiding I/O during the search. Only loads the target block from
 * disk.
 *
 * Post-condition on `return true`: ts->cur_doc_id >= target_doc_id.
 *
 * Both the in-block linear scan and the binary-search-selected block can
 * exhaust without finding doc >= target if the cached skip data
 * (skip_entry.last_doc_id, block_last_doc_ids[]) is inconsistent with
 * on-disk block_postings -- a condition observed on MS MARCO under
 * concurrent-insert segment topology. In that case we keep loading
 * subsequent blocks until we find one whose linear scan succeeds, or we
 * exhaust the iterator. Without this guarantee, callers like seek_to_pivot
 * spin forever (gdb stack on the production hang showed
 * seek_to_pivot:1329 with cur_doc_id < pivot_doc_id after a `return true`
 * from this function).
 */
static bool
seek_term_to_doc(TpTermState *ts, uint32 target_doc_id)
{
	uint32 block_count;
	uint32 left, right, mid;
	uint32 target_block;

	if (!ts->found || ts->iter.finished)
		return false;
	Assert(target_doc_id >= ts->cur_doc_id);
	ts->seek_pending = false;

	/*
	 * Fast path: target is in (or before) the current block, per the
	 * cached last_doc_id. Linear-scan from current_in_block.
	 *
	 * If the cached last_doc_id is accurate, the loop below will find a
	 * doc >= target before exhausting the block. If it is *not* accurate
	 * (i.e., last_doc_id >= target but no posting in the block is >=
	 * target), the loop falls through and the block-advancing loop below
	 * picks up where we left off.
	 */
	if (target_doc_id <= ts->iter.skip_entry.last_doc_id)
	{
		while (ts->iter.current_in_block < ts->iter.skip_entry.doc_count)
		{
			uint32 doc_id =
					ts->iter.block_postings[ts->iter.current_in_block].doc_id;
			if (doc_id >= target_doc_id)
			{
				ts->cur_doc_id = doc_id;
				return true;
			}
			ts->iter.current_in_block++;
		}
		/* Fall through: advance past current block. */
	}
	else
	{
		/*
		 * Target is past current block's cached last_doc_id. Binary
		 * search the per-term skip cache to find the first block whose
		 * last_doc_id >= target.
		 */
		block_count = ts->iter.dict_entry.block_count;
		left		= ts->iter.current_block + 1;
		right		= left;
		if (left < block_count && ts->block_last_doc_ids[left] < target_doc_id)
			right = block_count - 1;

		while (left < right)
		{
			mid = left + (right - left) / 2;
			if (ts->block_last_doc_ids[mid] < target_doc_id)
				left = mid + 1;
			else
				right = mid;
		}
		target_block = left;

		if (target_block >= block_count)
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}

		/* Position at the start of the binary-search-selected block. */
		ts->iter.current_block	  = target_block;
		ts->iter.current_in_block = 0;
		if (!tp_segment_posting_iterator_load_block(&ts->iter))
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
	}

	/*
	 * Block-advancing scan loop. Keep loading subsequent blocks until
	 * we find a posting >= target_doc_id or exhaust the iterator.
	 *
	 * This loop is the durable post-condition guarantor: it terminates
	 * only by finding doc >= target (and returning true) or by
	 * exhausting all blocks (and returning false). It does not rely on
	 * cached skip data being consistent with block_postings -- if a
	 * block's actual postings are all < target despite a cached
	 * last_doc_id >= target, we simply move on to the next block.
	 *
	 * Termination: each iteration of the outer loop strictly increments
	 * current_block, bounded by dict_entry.block_count. The inner
	 * while-loop scans a single block, bounded by skip_entry.doc_count.
	 */
	for (;;)
	{
		/*
		 * Cancelability: each iteration of the outer loop calls
		 * tp_segment_posting_iterator_load_block, which performs disk
		 * I/O and possibly block decompression. Under the cache-
		 * inconsistency scenario this function defends against, the
		 * loop can iterate across many blocks. Without this CHECK,
		 * statement_timeout / pg_cancel_backend cannot abort the
		 * scan, and seek_to_pivot's CHECK_FOR_INTERRUPTS one level up
		 * never fires because we never return from this function.
		 */
		CHECK_FOR_INTERRUPTS();

		while (ts->iter.current_in_block < ts->iter.skip_entry.doc_count)
		{
			uint32 doc_id =
					ts->iter.block_postings[ts->iter.current_in_block].doc_id;
			if (doc_id >= target_doc_id)
			{
				ts->cur_doc_id = doc_id;
				return true;
			}
			ts->iter.current_in_block++;
		}

		ts->iter.current_block++;
		if (ts->iter.current_block >= ts->iter.dict_entry.block_count)
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
		ts->iter.current_in_block = 0;
		if (!tp_segment_posting_iterator_load_block(&ts->iter))
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
	}
}

/*
 * Initialize term states for a segment.
 * Returns count of active iterators (terms found in segment).
 * Terms array is sorted by doc ID after initialization.
 */
static int
init_segment_term_states(
		TpTermState	   **terms,
		int				 term_count,
		TpSegmentReader *reader,
		float4			 k1,
		float4			 b,
		float4			 avg_doc_len)
{
	int active_count = 0;
	int term_idx;

	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState *ts = terms[term_idx];

		ts->found			   = false;
		ts->max_score		   = 0.0f;
		ts->cur_doc_id		   = UINT32_MAX;
		ts->block_max_scores   = NULL;
		ts->block_last_doc_ids = NULL;
		ts->bound_block		   = 0;
		ts->seek_pending	   = false;

		if (!tp_segment_posting_iterator_init(&ts->iter, reader, ts->term))
			continue;

		ts->found = true;

		/* Pre-load skip entries for BMW threshold checks and fast seeking */
		if (ts->iter.dict_entry.block_count > 0)
		{
			uint32		 block_idx;
			uint32		 block_count = ts->iter.dict_entry.block_count;
			TpSkipEntry *skip_cache;

			ts->block_max_scores   = palloc(block_count * sizeof(float4));
			ts->block_last_doc_ids = palloc(block_count * sizeof(uint32));

			/* Allocate and populate skip entry cache for the iterator */
			skip_cache = palloc(block_count * sizeof(TpSkipEntry));

			for (block_idx = 0; block_idx < block_count; block_idx++)
			{
				tp_segment_read_skip_entry(
						reader,
						ts->iter.dict_entry.skip_index_offset,
						block_idx,
						&skip_cache[block_idx]);
				ts->block_max_scores[block_idx] = tp_compute_block_max_score(
						&skip_cache[block_idx], ts->idf, k1, b, avg_doc_len);
				ts->block_last_doc_ids[block_idx] =
						skip_cache[block_idx].last_doc_id;

				if (ts->block_max_scores[block_idx] > ts->max_score)
					ts->max_score = ts->block_max_scores[block_idx];
			}

			ts->max_score *= ts->query_freq;

			/* Set caches on iterator for load_block to use */
			ts->iter.cached_skip_entries  = skip_cache;
			ts->iter.compressed_buf_cache = palloc(
					TP_MAX_COMPRESSED_BLOCK_SIZE);
		}

		if (tp_segment_posting_iterator_load_block(&ts->iter))
		{
			refresh_cur_doc_id(ts);
			active_count++;
		}
	}

	return active_count;
}

/*
 * Free term state resources for a segment.
 */
static void
cleanup_segment_term_states(TpTermState **terms, int term_count)
{
	int term_idx;

	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState *ts = terms[term_idx];

		/*
		 * Free BMW-owned caches before iterator_free (which NULLs
		 * the borrowed pointers but doesn't free them).
		 */
		if (ts->iter.cached_skip_entries)
			pfree(ts->iter.cached_skip_entries);
		if (ts->iter.compressed_buf_cache)
			pfree(ts->iter.compressed_buf_cache);

		if (ts->found)
			tp_segment_posting_iterator_free(&ts->iter);
		if (ts->block_max_scores)
			pfree(ts->block_max_scores);
		if (ts->block_last_doc_ids)
			pfree(ts->block_last_doc_ids);
	}
}

/*
 * Compare terms by current doc_id for initial sort.
 * Exhausted terms (UINT32_MAX) sort to the end.
 */
static int
compare_term_doc_id(const void *a, const void *b)
{
	TpTermState *const *pa = (TpTermState *const *)a;
	TpTermState *const *pb = (TpTermState *const *)b;
	uint32				da = term_current_doc_id(*pa);
	uint32				db = term_current_doc_id(*pb);

	if (da < db)
		return -1;
	if (da > db)
		return 1;
	return 0;
}

/*
 * Sort term states by current doc_id.
 * Used once after init_segment_term_states.
 */
static void
sort_terms_by_doc_id(TpTermState **terms, int term_count)
{
	qsort(terms, term_count, sizeof(TpTermState *), compare_term_doc_id);
}

/*
 * Restore sorted order after term at position 'ord' advanced.
 * The term's doc_id increased, so it may need to move right.
 * Uses linear insertion -- O(1) typical, O(T) worst case.
 */
static void
restore_ordering(TpTermState **terms, int term_count, int ord)
{
	TpTermState *tmp;
	uint32		 doc_id = term_current_doc_id(terms[ord]);
	int			 i;

	for (i = ord + 1; i < term_count; i++)
	{
		if (term_current_doc_id(terms[i]) >= doc_id)
			break;
	}

	/* Term needs to move from ord to i-1 */
	if (i > ord + 1)
	{
		tmp = terms[ord];
		memmove(&terms[ord],
				&terms[ord + 1],
				(i - ord - 1) * sizeof(TpTermState *));
		terms[i - 1] = tmp;
	}
}

/*
 * WAND pivot selection.
 *
 * Given terms sorted by current doc_id, walk from lowest doc_id
 * accumulating each term's max_score. When the sum exceeds
 * threshold, we've found the pivot.
 *
 * Returns true if a pivot was found, false if no term combination
 * can beat threshold. Sets *pivot_len_out to the number of terms
 * participating (terms[0..pivot_len-1]) and *pivot_doc_id_out to
 * the pivot document.
 */
static bool
find_wand_pivot(
		TpTermState **terms,
		int			  term_count,
		int			  required_count,
		float4		  threshold,
		int			 *pivot_len_out,
		uint32		 *pivot_doc_id_out)
{
	float4 accumulated = 0.0f;
	int	   i;

	for (i = 0; i < term_count; i++)
	{
		uint32 doc_id = term_current_doc_id(terms[i]);
		if (doc_id == UINT32_MAX)
			break; /* No more active terms */

		accumulated += terms[i]->max_score;
		if (terms[i]->required)
			required_count--;
		if (accumulated > threshold && required_count == 0)
		{
			/*
			 * Found pivot: terms[i] is the pivot term.
			 * Include any subsequent terms at the same doc_id.
			 */
			uint32 pivot_doc = doc_id;
			int	   pivot_len = i + 1;

			while (pivot_len < term_count &&
				   term_current_doc_id(terms[pivot_len]) == pivot_doc)
				pivot_len++;

			*pivot_len_out	  = pivot_len;
			*pivot_doc_id_out = pivot_doc;
			return true;
		}
	}

	return false; /* Can't beat threshold */
}

static uint32
shallow_seek_block(TpTermState *ts, uint32 target)
{
	uint32 left	 = Max(ts->bound_block, ts->iter.current_block);
	uint32 right = ts->iter.dict_entry.block_count;

	if (left < right && ts->block_last_doc_ids[left] >= target)
		return ts->bound_block = left;
	if (left < right)
		left++;
	while (left < right)
	{
		uint32 mid = left + (right - left) / 2;
		if (ts->block_last_doc_ids[mid] < target)
			left = mid + 1;
		else
			right = mid;
	}
	ts->bound_block = left;
	return left;
}

/*
 * Bound the complete score over [pivot_doc_id, min_block_end] before
 * loading postings. Every term, including non-pivot terms (#365), has
 * a cached block covering that range, or starts beyond it.
 * WAND already excluded documents before the pivot.
 *
 * A rejected range advances every term's lower bound past its end.
 * Posting iterators catch up only when a competitive pivot needs them.
 * Advancing the earliest-ending block guarantees progress (#355).
 */
static bool
shallow_skip(
		TpTermState **terms,
		int			  term_count,
		int			  pivot_len,
		uint32		  pivot_doc_id,
		float4		  threshold,
		int			 *active_count,
		uint32		 *bound_end,
		float8		 *bound_score,
		TpBMWStats	 *stats)
{
	uint32 min_block_end = UINT32_MAX;
	uint32 range_end	 = pivot_len < *active_count
								 ? term_current_doc_id(terms[pivot_len]) - 1
								 : UINT32_MAX;
	float8 upper_bound	 = 0.0;
	float8 loaded_bound	 = 0.0;
	uint32 loaded_end	 = UINT32_MAX;
	bool   exhausted	 = false;
	int	   i;

	if (threshold <= 0.0f || term_count * FLT_EPSILON >= 0.5)
		return false;
	min_block_end = range_end;

	/* Loaded pivot blocks may already prove that no shallow skip is useful. */
	for (i = 0; i < pivot_len; i++)
	{
		TpTermState *ts = terms[i];
		if (ts->block_max_scores == NULL)
			return false;
		if (ts->iter.skip_entry.last_doc_id < pivot_doc_id)
			continue;
		loaded_bound += ts->block_max_scores[ts->iter.current_block] *
						ts->query_freq;
		loaded_end = Min(loaded_end, ts->iter.skip_entry.last_doc_id);
		if (loaded_bound >= threshold)
		{
			*bound_end	 = Min(loaded_end, range_end);
			*bound_score = loaded_bound;
			return false;
		}
	}

	for (i = 0; i < pivot_len; i++)
	{
		TpTermState *ts = terms[i];
		uint32		 block;

		if (!ts->found || ts->iter.finished)
			continue;
		if (term_current_doc_id(ts) > range_end)
			continue;
		if (ts->block_last_doc_ids == NULL || ts->block_max_scores == NULL)
			return false;
		block = shallow_seek_block(
				ts, Max(pivot_doc_id, term_current_doc_id(ts)));
		if (block >= ts->iter.dict_entry.block_count)
		{
			ts->iter.finished = true;
			ts->seek_pending  = false;
			ts->cur_doc_id	  = UINT32_MAX;
			(*active_count)--;
			restore_ordering(terms, term_count, i);
			return true;
		}
		if (ts->block_last_doc_ids[block] < min_block_end)
			min_block_end = ts->block_last_doc_ids[block];
	}

	if (min_block_end == UINT32_MAX)
		return false;
	for (i = 0; i < pivot_len; i++)
	{
		TpTermState *ts = terms[i];
		if (!ts->found || ts->iter.finished ||
			term_current_doc_id(ts) > min_block_end)
			continue;
		upper_bound += ts->block_max_scores[ts->bound_block] * ts->query_freq;
	}
	/* Allow score-sum roundoff; equal scores may win via CTID tie-breaking. */
	upper_bound *= 1.0 + 2.0 * term_count * FLT_EPSILON;
	*bound_end	 = min_block_end;
	*bound_score = upper_bound;
	if (upper_bound >= threshold)
		return false;

	for (i = 0;
		 i < term_count && term_current_doc_id(terms[i]) <= min_block_end;
		 i++)
	{
		TpTermState *ts = terms[i];
		if (ts->bound_block + 1 >= ts->iter.dict_entry.block_count &&
			ts->block_last_doc_ids[ts->bound_block] <= min_block_end)
		{
			ts->iter.finished = true;
			ts->seek_pending  = false;
			ts->cur_doc_id	  = UINT32_MAX;
			(*active_count)--;
			exhausted = true;
		}
		else
		{
			ts->cur_doc_id	 = min_block_end + 1;
			ts->seek_pending = true;
		}
	}
	if (stats != NULL)
		stats->blocks_skipped++;
	if (exhausted)
		sort_terms_by_doc_id(terms, term_count);
	return true;
}

/*
 * Seek pre-pivot terms to pivot_doc_id.
 * Returns true if all terms are still active, false if a term
 * was exhausted (caller should re-pivot).
 */
static bool
seek_to_pivot(
		TpTermState **terms,
		int			  term_count,
		int			  pivot_len,
		uint32		  pivot_doc_id,
		int			 *active_count)
{
	int i;

	for (i = 0; i < pivot_len; i++)
	{
		uint32 doc_id;

		/*
		 * Cancelability: the for loop's `i--; continue;` re-entry
		 * pattern, combined with restore_ordering rotating terms
		 * through slot `i`, was the gdb-observed spin site for the
		 * MS MARCO bucket-8 hang. The underlying cause (seek_term_to_doc
		 * returning true without advancing past target) is now fixed at
		 * the source, but keep this CHECK so any future regression in
		 * BMW invariants is interruptible from SQL rather than
		 * requiring SIGKILL. (The outer WAND loop's CFI is too coarse:
		 * if seek_to_pivot fails to terminate it never returns there.)
		 */
		CHECK_FOR_INTERRUPTS();

		doc_id = term_current_doc_id(terms[i]);

		if (doc_id == UINT32_MAX)
		{
			(*active_count)--;
			return false;
		}

		if (doc_id < pivot_doc_id ||
			(doc_id == pivot_doc_id && terms[i]->seek_pending))
		{
			if (!seek_term_to_doc(terms[i], pivot_doc_id))
			{
				(*active_count)--;
				restore_ordering(terms, term_count, i);
				return false;
			}
			restore_ordering(terms, term_count, i);
			i--; /* Re-check: a new term slid into position i */
			continue;
		}
	}

	return true;
}

/*
 * Verify all pivot terms are aligned at pivot_doc_id.
 * Seeks may have overshot; if any term is past pivot, return false
 * so the caller can re-pivot.
 */
static bool
verify_pivot_alignment(TpTermState **terms, int pivot_len, uint32 pivot_doc_id)
{
	int i;

	for (i = 0; i < pivot_len; i++)
	{
		if (term_current_doc_id(terms[i]) != pivot_doc_id)
			return false;
	}
	return true;
}

/*
 * Score pivot document by accumulating BM25 contributions from
 * all confirmed pivot terms.
 *
 * Only called after verify_pivot_alignment confirms all pivot
 * terms are positioned at pivot_doc_id, so each term's current
 * posting is guaranteed to be the pivot document.
 */
static float4
score_pivot_document(
		TpTermState **terms,
		int			  pivot_len,
		float4		  k1,
		float4		  b,
		float4		  avg_doc_len)
{
	float4 doc_score = 0.0f;
	int	   i;

	for (i = 0; i < pivot_len; i++)
	{
		TpTermState	   *ts = terms[i];
		TpBlockPosting *bp;
		float4			term_score;

		if (!ts->found || ts->iter.finished)
			continue;

		bp		   = &ts->iter.block_postings[ts->iter.current_in_block];
		term_score = compute_bm25_score(
							 ts->idf,
							 bp->frequency,
							 (int32)decode_fieldnorm(bp->fieldnorm),
							 k1,
							 b,
							 avg_doc_len) *
					 ts->query_freq;
		doc_score += term_score;
	}

	return doc_score;
}

/*
 * Score segment postings for multiple terms using WAND traversal.
 *
 * Classic WAND: terms are sorted by current doc_id. We walk from
 * lowest doc_id, accumulating max_scores until the threshold is
 * exceeded to find the "pivot" document. This skips large doc_id
 * ranges that can't contribute to top-k results.
 *
 * Shallow block bounds reject noncompetitive intervals before seeking
 * the real posting iterators to the pivot.
 */
static void
score_segment_multi_term_bmw(
		TpTopKHeap		*heap,
		TpSegmentReader *reader,
		TpTermState	   **terms,
		int				 term_count,
		float4			 k1,
		float4			 b,
		float4			 avg_doc_len,
		TpBMWStats		*stats,
		TpBMWFilter		*filter)
{
	int	   active_count;
	int	   required_count	   = 0;
	uint32 shallow_bound_end   = 0;
	float8 shallow_bound_score = 0.0;
	bool   drive_candidates = filter != NULL && filter->segment_next != NULL;

	for (int i = 0; i < term_count; i++)
		required_count += terms[i]->required;

	if (drive_candidates)
	{
		uint32 candidate;
		drive_candidates =
				filter->segment_next(filter->state, reader, 0, &candidate);
		if (drive_candidates && candidate == UINT32_MAX)
			return;
	}

	active_count = init_segment_term_states(
			terms, term_count, reader, k1, b, avg_doc_len);

	if (active_count == 0)
	{
		cleanup_segment_term_states(terms, term_count);
		return;
	}

	/* Sort terms by current doc_id for WAND traversal */
	sort_terms_by_doc_id(terms, term_count);

	/* WAND main loop */
	while (active_count > 0)
	{
		int	   pivot_len;
		uint32 pivot_doc_id;
		float4 threshold;
		float4 doc_score;
		int	   i;

		CHECK_FOR_INTERRUPTS();

		if (drive_candidates)
		{
			uint32 minimum = term_current_doc_id(terms[0]);
			uint32 candidate;
			if (!filter->segment_next(
						filter->state, reader, minimum, &candidate))
				drive_candidates = false;
			else if (candidate == UINT32_MAX)
				break;
			else if (candidate > minimum)
			{
				for (i = 0; i < term_count; i++)
				{
					if (term_current_doc_id(terms[i]) < candidate)
					{
						if (!seek_term_to_doc(terms[i], candidate))
							active_count--;
						if (stats)
							stats->seeks_performed++;
					}
				}
				sort_terms_by_doc_id(terms, term_count);
				continue;
			}
		}

		threshold = tp_topk_threshold(heap);

		/* Step 1: Find WAND pivot */
		if (!find_wand_pivot(
					terms,
					term_count,
					required_count,
					threshold,
					&pivot_len,
					&pivot_doc_id))
			break; /* No term combination can beat threshold */

		/*
		 * Defer redundant pruning checks within a competitive block range.
		 * This cache only bypasses pruning; it never rejects a document.
		 */
		if (threshold > 0.0f &&
			(pivot_doc_id > shallow_bound_end ||
			 threshold > shallow_bound_score) &&
			shallow_skip(
					terms,
					term_count,
					pivot_len,
					pivot_doc_id,
					threshold,
					&active_count,
					&shallow_bound_end,
					&shallow_bound_score,
					stats))
			continue;

		/* Step 2: Seek pre-pivot terms to pivot_doc_id */
		if (!seek_to_pivot(
					terms, term_count, pivot_len, pivot_doc_id, &active_count))
			continue; /* Re-pivot with updated positions */

		/* A required term may have sought beyond the proposed pivot. */
		if (!verify_pivot_alignment(terms, pivot_len, pivot_doc_id))
			continue;

		if (stats)
			stats->blocks_scanned++;

		/* Skip dead docs */
		if (!tp_segment_is_alive(reader, pivot_doc_id))
		{
			if (stats)
				stats->dead_docs_skipped++;
		}
		else
		{
			/* Step 5: Score the pivot document */
			doc_score =
					score_pivot_document(terms, pivot_len, k1, b, avg_doc_len);

			if (doc_score > 0.0f && !tp_topk_dominated(heap, doc_score) &&
				(filter == NULL ||
				 filter->segment_matches(filter->state, reader, pivot_doc_id)))
				tp_topk_add_segment(
						heap, reader->root_block, pivot_doc_id, doc_score);

			if (stats)
				stats->segment_docs_scored++;
		}

		/*
		 * Step 6: Advance all pivot terms past pivot_doc_id.
		 * Iterate backward so restore_ordering shifts don't
		 * skip any terms.
		 */
		for (i = pivot_len - 1; i >= 0; i--)
		{
			if (term_current_doc_id(terms[i]) == pivot_doc_id)
			{
				if (!advance_term_iterator(terms[i]))
					active_count--;
				restore_ordering(terms, term_count, i);
			}
		}
	}

	cleanup_segment_term_states(terms, term_count);
}

/*
 * Score documents using multi-term Block-Max WAND.
 */
int
tp_score_multi_term_bmw(
		TpLocalIndexState			 *local_state,
		Relation					  index,
		const TpSegmentGraphSnapshot *snapshot,
		TpDataSource				 *memtable_src,
		char						**query_terms,
		int							  term_count,
		int32						 *query_freqs,
		float4						 *idfs,
		float4						  k1,
		float4						  b,
		float4						  avg_doc_len,
		int							  max_results,
		ItemPointerData				 *result_ctids,
		float4						 *result_scores,
		TpBMWStats					 *stats,
		TpBMWFilter					 *filter)
{
	TpTopKHeap	  heap;
	TpTermState **terms;
	int			  result_count;
	int			  i;

	(void)local_state; /* reserved for future use */

	/* Initialize stats */
	if (stats)
		memset(stats, 0, sizeof(TpBMWStats));

	/* Initialize top-k heap */
	tp_topk_init(&heap, max_results, CurrentMemoryContext);

	/* Initialize term states */
	terms = palloc(term_count * sizeof(TpTermState *));
	for (i = 0; i < term_count; i++)
	{
		terms[i]			 = palloc(sizeof(TpTermState));
		terms[i]->term		 = query_terms[i];
		terms[i]->idf		 = idfs[i];
		terms[i]->query_freq = query_freqs[i];
		terms[i]->required	 = false;
		if (filter != NULL)
		{
			for (int j = 0; j < filter->term_count; j++)
			{
				if (filter->required[j] &&
					strcmp(query_terms[i], filter->terms[j]) == 0)
				{
					terms[i]->required = true;
					break;
				}
			}
		}
	}

	/* Score memtable (exhaustive - no skip index) */
	score_memtable_multi_term(
			&heap,
			memtable_src,
			terms,
			term_count,
			k1,
			b,
			avg_doc_len,
			stats,
			filter);
	if (filter != NULL)
		filter->reset(filter->state);

	/* Score each segment with block-based BMW */
	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		uint32			   root_count;
		const BlockNumber *roots =
				tp_segment_graph_snapshot_level(snapshot, level, &root_count);

		for (uint32 root_idx = 0; root_idx < root_count; root_idx++)
		{
			TpSegmentReader *reader = tp_segment_open(index, roots[root_idx]);

			CHECK_FOR_INTERRUPTS();

			score_segment_multi_term_bmw(
					&heap,
					reader,
					terms,
					term_count,
					k1,
					b,
					avg_doc_len,
					stats,
					filter);
			if (filter != NULL)
				filter->reset(filter->state);

			tp_segment_close(reader);
		}
	}

	for (i = 0; i < term_count; i++)
		pfree(terms[i]);
	pfree(terms);

	/* Resolve CTIDs for segment results before extraction */
	tp_topk_resolve_ctids(&heap, index);

	/* Extract sorted results */
	result_count = tp_topk_extract(&heap, result_ctids, result_scores);

	if (stats)
		stats->docs_in_results = result_count;

	return result_count;
}
