/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * bmw.c - Block-Max WAND query optimization implementation
 */
#include <postgres.h>

#include <math.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>

#include "constants.h"
#include "memtable/source.h"
#include "query/bmw.h"
#include "query/score.h"
#include "segment/fieldnorm.h"
#include "source.h"
#include "state/metapage.h"

/* GUC variable for WAND-style seeking - defined in mod.c */
extern bool tp_enable_wand_seek;

/*
 * ------------------------------------------------------------
 * Top-K Min-Heap Implementation
 * ------------------------------------------------------------
 */

void
tp_topk_init(TpTopKHeap *heap, int k, MemoryContext ctx)
{
	MemoryContext old_ctx = MemoryContextSwitchTo(ctx);

	heap->ctids	   = palloc(k * sizeof(ItemPointerData));
	heap->scores   = palloc(k * sizeof(float4));
	heap->capacity = k;
	heap->size	   = 0;

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
	ItemPointerData tmp_ctid  = heap->ctids[i];
	float4			tmp_score = heap->scores[i];

	heap->ctids[i]	= heap->ctids[j];
	heap->scores[i] = heap->scores[j];
	heap->ctids[j]	= tmp_ctid;
	heap->scores[j] = tmp_score;
}

/*
 * Compare two heap entries for min-heap ordering.
 * Returns true if entry at position a should be the parent of entry at b.
 *
 * For min-heap with tie-breaking:
 * - Lower score = should be parent (closer to root)
 * - For equal scores, higher CTID = should be parent (is "smaller")
 *
 * Why higher CTID is "smaller": Heapsort produces ascending order, then
 * we reverse for descending output. For equal scores we want lower CTIDs
 * first in output, so after reversal lower CTIDs should be at front.
 * Before reversal (ascending), higher CTIDs should come first.
 */
static inline bool
heap_less(TpTopKHeap *heap, int a, int b)
{
	if (heap->scores[a] != heap->scores[b])
		return heap->scores[a] < heap->scores[b];
	/* Tiebreaker: higher CTID is "smaller" for heap purposes */
	return ItemPointerCompare(&heap->ctids[a], &heap->ctids[b]) > 0;
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
		if (heap_less(heap, parent, i) ||
			(!heap_less(heap, i, parent) && parent <= i))
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

void
tp_topk_add(TpTopKHeap *heap, ItemPointerData ctid, float4 score)
{
	if (heap->size < heap->capacity)
	{
		/* Heap not full - just add */
		int i			= heap->size++;
		heap->ctids[i]	= ctid;
		heap->scores[i] = score;
		heap_sift_up(heap, i);
	}
	else if (
			score > heap->scores[0] ||
			(score == heap->scores[0] &&
			 ItemPointerCompare(&ctid, &heap->ctids[0]) < 0))
	{
		/*
		 * Heap full but new entry should replace root:
		 * - Score beats minimum, OR
		 * - Score equals minimum but new CTID is lower (we want to keep
		 *   docs with lower CTIDs, so evict higher CTID at root)
		 */
		heap->ctids[0]	= ctid;
		heap->scores[0] = score;
		heap_sift_down(heap, 0);
	}
	/* else: doesn't qualify for top-k, ignore */
}

/*
 * Extract sorted results (descending by score).
 * Uses heap-sort: repeatedly swap root with end.
 *
 * Min-heap heapsort produces descending order (max at front), which is
 * exactly what we want: highest scores first, lowest CTIDs first for ties.
 */
int
tp_topk_extract(TpTopKHeap *heap, ItemPointerData *ctids, float4 *scores)
{
	int count = heap->size;

	/* Heapsort in place */
	while (heap->size > 0)
	{
		heap->size--;
		heap_swap(heap, 0, heap->size);
		heap_sift_down(heap, 0);
	}

	/* Copy to output - heapsort already produced descending order */
	for (int i = 0; i < count; i++)
	{
		ctids[i]  = heap->ctids[i];
		scores[i] = heap->scores[i];
	}

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
 * Uses the TpDataSource interface for clean abstraction.
 */
static void
score_memtable_single_term(
		TpTopKHeap		  *heap,
		TpLocalIndexState *local_state,
		const char		  *term,
		float4			   idf,
		float4			   k1,
		float4			   b,
		float4			   avg_doc_len,
		TpBMWStats		  *stats)
{
	TpDataSource  *source;
	TpPostingData *postings;
	int			   i;

	/* Create memtable data source */
	source = tp_memtable_source_create(local_state);
	if (!source)
		return;

	/* Get postings for this term */
	postings = tp_source_get_postings(source, term);
	if (!postings || postings->count == 0)
	{
		if (postings)
			tp_source_free_postings(source, postings);
		tp_source_close(source);
		return;
	}

	/* Score each posting */
	for (i = 0; i < postings->count; i++)
	{
		ItemPointerData *ctid = &postings->ctids[i];
		int32			 tf	  = postings->frequencies[i];
		int32			 doc_len;
		float4			 score;

		/* Get document length */
		doc_len = tp_source_get_doc_length(source, ctid);
		if (doc_len <= 0)
			doc_len = 1; /* Fallback for missing entries */

		score = compute_bm25_score(idf, tf, doc_len, k1, b, avg_doc_len);

		if (!tp_topk_dominated(heap, score))
			tp_topk_add(heap, *ctid, score);

		if (stats)
			stats->memtable_docs++;
	}

	tp_source_free_postings(source, postings);
	tp_source_close(source);
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
		TpBMWStats		*stats)
{
	TpSegmentPostingIterator iter;
	TpSegmentPosting		*posting;
	TpDictEntry				*dict_entry;
	uint16					 block_count;
	float4					*block_max_scores;
	uint16					 i;

	/* Initialize iterator for this term */
	if (!tp_segment_posting_iterator_init(&iter, reader, term))
		return; /* Term not found in segment */

	/* Get dictionary entry for block count and skip index */
	dict_entry	= &iter.dict_entry;
	block_count = dict_entry->block_count;

	/* Pre-compute block max scores */
	block_max_scores = palloc(block_count * sizeof(float4));
	for (i = 0; i < block_count; i++)
	{
		TpSkipEntry skip;
		tp_segment_read_skip_entry(reader, dict_entry, i, &skip);
		block_max_scores[i] =
				tp_compute_block_max_score(&skip, idf, k1, b, avg_doc_len);
	}

	/* Process blocks with BMW */
	for (i = 0; i < block_count; i++)
	{
		float4 threshold = tp_topk_threshold(heap);
		float4 block_max = block_max_scores[i];

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

			score = compute_bm25_score(
					idf,
					posting->frequency,
					posting->doc_length,
					k1,
					b,
					avg_doc_len);

			if (!tp_topk_dominated(heap, score))
			{
				tp_topk_add(heap, posting->ctid, score);
			}

			if (stats)
				stats->segment_docs_scored++;
		}
	}

	pfree(block_max_scores);
	tp_segment_posting_iterator_free(&iter);
}

int
tp_score_single_term_bmw(
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
		TpBMWStats		  *stats)
{
	TpTopKHeap		heap;
	TpIndexMetaPage metap;
	BlockNumber		level_heads[TP_MAX_LEVELS];
	int				level;
	int				result_count;

	/* Initialize stats */
	if (stats)
		memset(stats, 0, sizeof(TpBMWStats));

	/* Initialize top-k heap */
	tp_topk_init(&heap, max_results, CurrentMemoryContext);

	/* Score memtable (exhaustive - no skip index) */
	score_memtable_single_term(
			&heap, local_state, term, idf, k1, b, avg_doc_len, stats);

	/* Get segment level heads from metapage */
	metap = tp_get_metapage(index);
	for (level = 0; level < TP_MAX_LEVELS; level++)
		level_heads[level] = metap->level_heads[level];
	pfree(metap);

	/* Score each segment level with BMW */
	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber seg_head = level_heads[level];

		while (seg_head != InvalidBlockNumber)
		{
			TpSegmentReader *reader = tp_segment_open(index, seg_head);

			score_segment_single_term_bmw(
					&heap, reader, term, idf, k1, b, avg_doc_len, stats);

			seg_head = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}

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

	/* Segment-specific state (reset per segment) */
	bool					 found; /* Term found in current segment */
	TpSegmentPostingIterator iter;	/* Iterator (contains dict_entry) */
	float4 *block_max_scores;		/* Pre-computed block max scores */
	uint32 *block_last_doc_ids;		/* Cached last_doc_id per block */
} TpTermState;

/*
 * Get current doc ID for a term, or UINT32_MAX if exhausted.
 */
static inline uint32
term_current_doc_id(TpTermState *ts)
{
	if (!ts->found || ts->iter.finished)
		return UINT32_MAX;
	return tp_segment_posting_iterator_current_doc_id(&ts->iter);
}

/*
 * Score memtable postings for multiple terms.
 * Memtable has no skip index, so we score all postings exhaustively.
 */
static void
score_memtable_multi_term(
		TpTopKHeap		  *heap,
		TpLocalIndexState *local_state,
		TpTermState		  *terms,
		int				   term_count,
		float4			   k1,
		float4			   b,
		float4			   avg_doc_len,
		TpBMWStats		  *stats)
{
	TpDataSource *source;
	HTAB		 *doc_accum;
	HASHCTL		  hash_ctl;
	int			  term_idx;

	/* Create memtable data source */
	source = tp_memtable_source_create(local_state);
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
		TpTermState	  *ts = &terms[term_idx];
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

			/* Get document length */
			doc_len = tp_source_get_doc_length(source, ctid);
			if (doc_len <= 0)
				doc_len = 1;

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
			if (!tp_topk_dominated(heap, entry->score))
				tp_topk_add(heap, entry->ctid, entry->score);

			if (stats)
				stats->memtable_docs++;
		}
	}

	hash_destroy(doc_accum);
	tp_source_close(source);
}

/*
 * Advance a term iterator to the next document.
 * Returns true if iterator is still active, false if exhausted.
 */
static bool
advance_term_iterator(TpTermState *ts)
{
	ts->iter.current_in_block++;

	if (ts->iter.current_in_block >= ts->iter.skip_entry.doc_count)
	{
		ts->iter.current_block++;
		if (ts->iter.current_block >= ts->iter.dict_entry.block_count)
		{
			ts->iter.finished = true;
			return false;
		}
		ts->iter.current_in_block = 0;
		tp_segment_posting_iterator_load_block(&ts->iter);
	}
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
 */
static bool
seek_term_to_doc(TpTermState *ts, uint32 target_doc_id)
{
	uint16 block_count;
	int	   left, right, mid;
	uint16 target_block;

	if (!ts->found || ts->iter.finished)
		return false;

	/*
	 * Check if target is in current block - if so, linear scan is faster
	 * than the seek overhead.
	 */
	if (target_doc_id <= ts->iter.skip_entry.last_doc_id)
	{
		/* Target is in current block, linear scan within block */
		while (ts->iter.current_in_block < ts->iter.skip_entry.doc_count)
		{
			uint32 doc_id =
					ts->iter.block_postings[ts->iter.current_in_block].doc_id;
			if (doc_id >= target_doc_id)
				return true;
			ts->iter.current_in_block++;
		}
		/* Exhausted current block, fall through to load next */
		ts->iter.current_block++;
		if (ts->iter.current_block >= ts->iter.dict_entry.block_count)
		{
			ts->iter.finished = true;
			return false;
		}
		ts->iter.current_in_block = 0;
		tp_segment_posting_iterator_load_block(&ts->iter);
		return !ts->iter.finished;
	}

	/*
	 * Target is beyond current block - binary search on cached last_doc_ids.
	 * This is pure in-memory search, no I/O until we load the target block.
	 */
	block_count = ts->iter.dict_entry.block_count;

	/* Binary search: find first block where last_doc_id >= target */
	left  = ts->iter.current_block + 1; /* Start after current block */
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

	/* Check if target is past all blocks */
	if (target_block >= block_count)
	{
		ts->iter.finished = true;
		return false;
	}

	/* Load the target block */
	ts->iter.current_block	  = target_block;
	ts->iter.current_in_block = 0;

	if (!tp_segment_posting_iterator_load_block(&ts->iter))
	{
		ts->iter.finished = true;
		return false;
	}

	/* Linear scan within block to find target or first doc >= target */
	while (ts->iter.current_in_block < ts->iter.skip_entry.doc_count)
	{
		uint32 doc_id =
				ts->iter.block_postings[ts->iter.current_in_block].doc_id;
		if (doc_id >= target_doc_id)
			return true;
		ts->iter.current_in_block++;
	}

	/* Target not in this block, try next */
	ts->iter.current_block++;
	if (ts->iter.current_block >= block_count)
	{
		ts->iter.finished = true;
		return false;
	}
	ts->iter.current_in_block = 0;
	tp_segment_posting_iterator_load_block(&ts->iter);
	return !ts->iter.finished;
}

/*
 * Initialize term states for a segment.
 * Returns count of active iterators (terms found in segment).
 * Terms array is sorted by doc ID after initialization.
 */
static int
init_segment_term_states(
		TpTermState		*terms,
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
		TpTermState *ts = &terms[term_idx];

		ts->found			   = false;
		ts->block_max_scores   = NULL;
		ts->block_last_doc_ids = NULL;

		if (!tp_segment_posting_iterator_init(&ts->iter, reader, ts->term))
			continue;

		ts->found = true;

		/* Pre-load skip entries for BMW threshold checks and fast seeking */
		if (ts->iter.dict_entry.block_count > 0)
		{
			uint16 block_idx;
			uint16 block_count = ts->iter.dict_entry.block_count;

			ts->block_max_scores   = palloc(block_count * sizeof(float4));
			ts->block_last_doc_ids = palloc(block_count * sizeof(uint32));

			for (block_idx = 0; block_idx < block_count; block_idx++)
			{
				TpSkipEntry skip;
				tp_segment_read_skip_entry(
						reader, &ts->iter.dict_entry, block_idx, &skip);
				ts->block_max_scores[block_idx] = tp_compute_block_max_score(
						&skip, ts->idf, k1, b, avg_doc_len);
				ts->block_last_doc_ids[block_idx] = skip.last_doc_id;
			}
		}

		if (tp_segment_posting_iterator_load_block(&ts->iter))
			active_count++;
	}

	return active_count;
}

/*
 * Free term state resources for a segment.
 */
static void
cleanup_segment_term_states(TpTermState *terms, int term_count)
{
	int term_idx;

	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		if (terms[term_idx].found)
			tp_segment_posting_iterator_free(&terms[term_idx].iter);
		if (terms[term_idx].block_max_scores)
			pfree(terms[term_idx].block_max_scores);
		if (terms[term_idx].block_last_doc_ids)
			pfree(terms[term_idx].block_last_doc_ids);
	}
}

/*
 * Find minimum doc_id across all active term iterators.
 * Returns UINT32_MAX if all iterators are exhausted.
 */
static uint32
find_pivot_doc_id(TpTermState *terms, int term_count)
{
	uint32 min_doc_id = UINT32_MAX;
	int	   term_idx;

	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		uint32 doc_id = term_current_doc_id(&terms[term_idx]);
		if (doc_id < min_doc_id)
			min_doc_id = doc_id;
	}
	return min_doc_id;
}

/*
 * Compute maximum possible score for a pivot document.
 * Uses block-max scores from current blocks of each term.
 */
static float4
compute_pivot_max_score(
		TpTermState *terms, int term_count, uint32 pivot_doc_id)
{
	float4 max_possible = 0.0f;
	int	   term_idx;

	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState *ts		= &terms[term_idx];
		uint32		 doc_id = term_current_doc_id(ts);

		/* Only include terms that are at or before the pivot */
		if (doc_id > pivot_doc_id)
			continue;

		if (ts->block_max_scores != NULL &&
			ts->iter.current_block < ts->iter.dict_entry.block_count)
		{
			max_possible += ts->block_max_scores[ts->iter.current_block] *
							ts->query_freq;
		}
	}

	return max_possible;
}

/*
 * Score a pivot document by accumulating BM25 contributions from all terms.
 * Advances iterators past the pivot and returns the score.
 * Sets *ctid_out to the document's CTID and *active_count is updated.
 */
static float4
score_pivot_document(
		TpTermState		*terms,
		int				 term_count,
		uint32			 pivot_doc_id,
		TpSegmentReader *reader,
		float4			 k1,
		float4			 b,
		float4			 avg_doc_len,
		ItemPointerData *ctid_out,
		int				*active_count)
{
	float4 doc_score = 0.0f;
	bool   have_ctid = false;
	int	   term_idx;

	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState	   *ts = &terms[term_idx];
		uint32			doc_id;
		TpBlockPosting *bp;
		float4			term_score;

		doc_id = term_current_doc_id(ts);
		if (doc_id != pivot_doc_id)
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

		if (!have_ctid)
		{
			Assert(reader->cached_ctid_pages != NULL);
			Assert(pivot_doc_id < reader->cached_num_docs);
			ItemPointerSet(
					ctid_out,
					reader->cached_ctid_pages[pivot_doc_id],
					reader->cached_ctid_offsets[pivot_doc_id]);
			have_ctid = true;
		}

		if (!advance_term_iterator(ts))
			(*active_count)--;
	}

	if (!have_ctid)
		ItemPointerSetInvalid(ctid_out);

	return doc_score;
}

/*
 * Find the minimum doc ID among terms NOT at the pivot.
 * Returns UINT32_MAX if all active terms are at the pivot.
 */
static uint32
find_next_candidate_doc_id(TpTermState *terms, int term_count, uint32 pivot)
{
	uint32 min_doc_id = UINT32_MAX;
	int	   term_idx;

	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		uint32 doc_id = term_current_doc_id(&terms[term_idx]);
		if (doc_id > pivot && doc_id < min_doc_id)
			min_doc_id = doc_id;
	}

	return min_doc_id;
}

/*
 * Advance all iterators at the pivot past the current document.
 *
 * When tp_enable_wand_seek is true (default), uses WAND-style seeking:
 * instead of advancing by 1, seek to the next candidate doc ID (minimum
 * among terms not at pivot). This is O(log blocks) instead of O(blocks).
 *
 * When tp_enable_wand_seek is false, advances one document at a time
 * (original behavior for comparison benchmarks).
 */
static void
skip_pivot_document(
		TpTermState *terms,
		int			 term_count,
		uint32		 pivot_doc_id,
		int			*active_count,
		TpBMWStats	*stats)
{
	uint32 next_candidate;
	int	   term_idx;

	if (tp_enable_wand_seek)
	{
		/* WAND-style: find next promising doc ID and seek to it */
		next_candidate =
				find_next_candidate_doc_id(terms, term_count, pivot_doc_id);
		if (next_candidate == UINT32_MAX)
			next_candidate = pivot_doc_id + 1;
	}
	else
	{
		/* Original behavior: advance by 1 */
		next_candidate = pivot_doc_id + 1;
	}

	/* Advance all terms at pivot to the next candidate */
	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState *ts		= &terms[term_idx];
		uint32		 doc_id = term_current_doc_id(ts);
		uint32		 skip_distance;

		if (doc_id != pivot_doc_id)
			continue;

		skip_distance = next_candidate - pivot_doc_id;

		if (tp_enable_wand_seek && skip_distance > 1)
		{
			/* Use binary search seek */
			if (!seek_term_to_doc(ts, next_candidate))
				(*active_count)--;

			if (stats)
			{
				uint32 landed_doc = term_current_doc_id(ts);
				stats->seeks_performed++;
				stats->docs_seeked += (next_candidate - pivot_doc_id - 1);
				if (landed_doc <= pivot_doc_id + 1)
					stats->seek_to_same_doc++;
			}
		}
		else
		{
			/* Linear advance */
			if (!advance_term_iterator(ts))
				(*active_count)--;

			if (stats)
				stats->linear_advances++;
		}
	}
}

/*
 * Score segment postings for multiple terms using WAND-style traversal.
 *
 * Uses doc-ID ordered traversal to correctly accumulate scores across all
 * terms. Block-max optimization prunes documents that can't beat threshold.
 */
static void
score_segment_multi_term_bmw(
		TpTopKHeap		*heap,
		TpSegmentReader *reader,
		TpTermState		*terms,
		int				 term_count,
		float4			 k1,
		float4			 b,
		float4			 avg_doc_len,
		TpBMWStats		*stats)
{
	int active_count;

	active_count = init_segment_term_states(
			terms, term_count, reader, k1, b, avg_doc_len);

	/* WAND-style doc-ID ordered traversal */
	while (active_count > 0)
	{
		uint32			pivot_doc_id;
		float4			max_possible;
		float4			threshold;
		ItemPointerData pivot_ctid;
		float4			doc_score;

		pivot_doc_id = find_pivot_doc_id(terms, term_count);
		if (pivot_doc_id == UINT32_MAX)
			break;

		threshold = tp_topk_threshold(heap);
		max_possible =
				compute_pivot_max_score(terms, term_count, pivot_doc_id);

		if (max_possible < threshold)
		{
			skip_pivot_document(
					terms, term_count, pivot_doc_id, &active_count, stats);
			if (stats)
				stats->blocks_skipped++;
			continue;
		}

		if (stats)
			stats->blocks_scanned++;

		doc_score = score_pivot_document(
				terms,
				term_count,
				pivot_doc_id,
				reader,
				k1,
				b,
				avg_doc_len,
				&pivot_ctid,
				&active_count);

		if (ItemPointerIsValid(&pivot_ctid) &&
			!tp_topk_dominated(heap, doc_score))
			tp_topk_add(heap, pivot_ctid, doc_score);

		if (stats)
			stats->segment_docs_scored++;
	}

	cleanup_segment_term_states(terms, term_count);
}

/*
 * Score documents using multi-term Block-Max WAND.
 */
int
tp_score_multi_term_bmw(
		TpLocalIndexState *local_state,
		Relation		   index,
		char			 **query_terms,
		int				   term_count,
		int32			  *query_freqs,
		float4			  *idfs,
		float4			   k1,
		float4			   b,
		float4			   avg_doc_len,
		int				   max_results,
		ItemPointerData	  *result_ctids,
		float4			  *result_scores,
		TpBMWStats		  *stats)
{
	TpTopKHeap		heap;
	TpIndexMetaPage metap;
	BlockNumber		level_heads[TP_MAX_LEVELS];
	TpTermState	   *terms;
	int				level;
	int				result_count;
	int				i;

	/* Initialize stats */
	if (stats)
		memset(stats, 0, sizeof(TpBMWStats));

	/* Initialize top-k heap */
	tp_topk_init(&heap, max_results, CurrentMemoryContext);

	/* Initialize term states */
	terms = palloc(term_count * sizeof(TpTermState));
	for (i = 0; i < term_count; i++)
	{
		terms[i].term		= query_terms[i];
		terms[i].idf		= idfs[i];
		terms[i].query_freq = query_freqs[i];
	}

	/* Score memtable (exhaustive - no skip index) */
	score_memtable_multi_term(
			&heap, local_state, terms, term_count, k1, b, avg_doc_len, stats);

	/* Get segment level heads from metapage */
	metap = tp_get_metapage(index);
	for (level = 0; level < TP_MAX_LEVELS; level++)
		level_heads[level] = metap->level_heads[level];
	pfree(metap);

	/* Score each segment with block-based BMW */
	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber seg_head = level_heads[level];

		while (seg_head != InvalidBlockNumber)
		{
			TpSegmentReader *reader = tp_segment_open(index, seg_head);

			score_segment_multi_term_bmw(
					&heap,
					reader,
					terms,
					term_count,
					k1,
					b,
					avg_doc_len,
					stats);

			seg_head = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}

	pfree(terms);

	/* Extract sorted results */
	result_count = tp_topk_extract(&heap, result_ctids, result_scores);

	if (stats)
		stats->docs_in_results = result_count;

	return result_count;
}
