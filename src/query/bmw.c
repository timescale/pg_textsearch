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
			stats->docs_scored++;
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
			/*
			 * Break if iterator auto-advanced to next block.
			 * This ensures we only process block i, allowing the outer
			 * for loop to apply threshold checks to subsequent blocks.
			 */
			if (iter.current_block != i)
				break;

			float4 score = compute_bm25_score(
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
				stats->docs_scored++;
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
 * Maximum number of terms for BMW multi-term path.
 * Beyond this, fall back to exhaustive scoring.
 */
#define BMW_MAX_TERMS 8

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
	float4					*block_max_scores; /* Pre-computed block max */
} TpTermState;

/*
 * Document accumulator for multi-term scoring within a block.
 * Uses a small hash table since blocks have at most 128 docs.
 */
typedef struct TpDocAccum
{
	uint32 doc_id;
	float4 score;
	uint8  fieldnorm; /* For doc length lookup */
} TpDocAccum;

#define DOC_ACCUM_HASH_SIZE 256 /* Power of 2 >= 128 * 2 */

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
				stats->docs_scored++;
		}
	}

	hash_destroy(doc_accum);
	tp_source_close(source);
}

/*
 * Score segment postings for multiple terms using BMW.
 * Skips blocks where sum of block_max_scores < threshold.
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
	uint16	max_blocks = 0;
	uint16	block_idx;
	int		term_idx;
	HTAB   *doc_accum;
	HASHCTL hash_ctl;

	/* Initialize term states for this segment */
	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState *ts = &terms[term_idx];

		ts->found			 = false;
		ts->block_max_scores = NULL;

		/* Initialize iterator (does dictionary lookup) */
		if (!tp_segment_posting_iterator_init(&ts->iter, reader, ts->term))
			continue;

		ts->found = true;

		/* Track maximum block count across all terms */
		if (ts->iter.dict_entry.block_count > max_blocks)
			max_blocks = ts->iter.dict_entry.block_count;

		/* Pre-compute block max scores for this term */
		ts->block_max_scores = palloc(
				ts->iter.dict_entry.block_count * sizeof(float4));
		for (block_idx = 0; block_idx < ts->iter.dict_entry.block_count;
			 block_idx++)
		{
			TpSkipEntry skip;
			tp_segment_read_skip_entry(
					reader, &ts->iter.dict_entry, block_idx, &skip);
			ts->block_max_scores[block_idx] = tp_compute_block_max_score(
					&skip, ts->idf, k1, b, avg_doc_len);
		}
	}

	/* If no terms found in segment, nothing to do */
	if (max_blocks == 0)
		goto cleanup;

	/* Create hash table for document score accumulation within blocks */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(uint32); /* doc_id */
	hash_ctl.entrysize = sizeof(TpDocAccum);
	hash_ctl.hcxt	   = CurrentMemoryContext;
	doc_accum		   = hash_create(
			 "Block Doc Accum",
			 DOC_ACCUM_HASH_SIZE,
			 &hash_ctl,
			 HASH_ELEM | HASH_BLOBS);

	/* Process blocks with BMW */
	for (block_idx = 0; block_idx < max_blocks; block_idx++)
	{
		float4 threshold	  = tp_topk_threshold(heap);
		float4 block_max_sum  = 0.0f;
		int	   terms_in_block = 0;

		/* Compute sum of block max scores for this block position */
		for (term_idx = 0; term_idx < term_count; term_idx++)
		{
			TpTermState *ts = &terms[term_idx];
			if (!ts->found || block_idx >= ts->iter.dict_entry.block_count)
				continue;

			block_max_sum += ts->block_max_scores[block_idx] * ts->query_freq;
			terms_in_block++;
		}

		/* Skip block if it can't beat threshold */
		if (block_max_sum < threshold || terms_in_block == 0)
		{
			if (stats && terms_in_block > 0)
				stats->blocks_skipped++;
			continue;
		}

		if (stats)
			stats->blocks_scanned++;

		/* Clear accumulator for this block */
		{
			HASH_SEQ_STATUS seq;
			TpDocAccum	   *entry;
			hash_seq_init(&seq, doc_accum);
			while ((entry = hash_seq_search(&seq)) != NULL)
				hash_search(doc_accum, &entry->doc_id, HASH_REMOVE, NULL);
		}

		/* Load and accumulate postings from all terms for this block */
		for (term_idx = 0; term_idx < term_count; term_idx++)
		{
			TpTermState		 *ts = &terms[term_idx];
			TpSegmentPosting *posting;

			if (!ts->found || block_idx >= ts->iter.dict_entry.block_count)
				continue;

			/* Load this block using the pre-initialized iterator */
			ts->iter.current_block = block_idx;
			ts->iter.finished	   = false; /* Reset for this block */
			tp_segment_posting_iterator_load_block(&ts->iter);

			/* Accumulate scores for all postings in block */
			while (tp_segment_posting_iterator_next(&ts->iter, &posting))
			{
				/* Break if iterator auto-advanced to next block */
				if (ts->iter.current_block != block_idx)
					break;
				float4			term_score;
				TpDocAccum	   *accum;
				bool			found;
				uint32			doc_id;
				ItemPointerData ctid_copy;

				/* Copy CTID to avoid alignment issues with packed struct */
				memcpy(&ctid_copy, &posting->ctid, sizeof(ItemPointerData));

				/* Use CTID as pseudo doc_id for hashing */
				doc_id = ItemPointerGetBlockNumber(&ctid_copy) * 65536 +
						 ItemPointerGetOffsetNumber(&ctid_copy);

				term_score = compute_bm25_score(
									 ts->idf,
									 posting->frequency,
									 posting->doc_length,
									 k1,
									 b,
									 avg_doc_len) *
							 ts->query_freq;

				accum = (TpDocAccum *)
						hash_search(doc_accum, &doc_id, HASH_ENTER, &found);

				if (!found)
				{
					accum->doc_id	 = doc_id;
					accum->score	 = term_score;
					accum->fieldnorm = 0;
				}
				else
				{
					accum->score += term_score;
				}
			}
		}

		/* Add accumulated documents to heap */
		{
			HASH_SEQ_STATUS seq;
			TpDocAccum	   *accum;

			hash_seq_init(&seq, doc_accum);
			while ((accum = hash_seq_search(&seq)) != NULL)
			{
				ItemPointerData ctid;

				/* Reconstruct CTID from doc_id encoding */
				ItemPointerSet(
						&ctid, accum->doc_id / 65536, accum->doc_id % 65536);

				if (!tp_topk_dominated(heap, accum->score))
					tp_topk_add(heap, ctid, accum->score);

				if (stats)
					stats->docs_scored++;
			}
		}
	}

	hash_destroy(doc_accum);

cleanup:
	/* Free iterators and block_max_scores */
	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		if (terms[term_idx].found)
			tp_segment_posting_iterator_free(&terms[term_idx].iter);
		if (terms[term_idx].block_max_scores)
			pfree(terms[term_idx].block_max_scores);
	}
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

	/* Limit term count for BMW path */
	if (term_count > BMW_MAX_TERMS)
		return -1; /* Signal caller to use exhaustive path */

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

	/* Score each segment with multi-term BMW */
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
