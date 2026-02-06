/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
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
 * Returns true if entry at position a should be the parent of entry at b.
 *
 * For min-heap: lower score = should be parent (closer to root).
 * Tie-breaking by CTID happens after CTIDs are resolved at extraction time.
 */
static inline bool
heap_less(TpTopKHeap *heap, int a, int b)
{
	return heap->scores[a] < heap->scores[b];
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
	else if (score > heap->scores[0])
	{
		/* Heap full but new entry beats minimum */
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
	else if (score > heap->scores[0])
	{
		/* Heap full but new entry beats minimum */
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
			tp_topk_add_memtable(heap, *ctid, score);

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
				tp_topk_add_segment(
						heap, reader->root_block, posting->doc_id, score);
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

	/* Global maximum score across all blocks (for WAND pivot) */
	float4 max_score;

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
				tp_topk_add_memtable(heap, entry->ctid, entry->score);

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
		ts->max_score		   = 0.0f;
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

				if (ts->block_max_scores[block_idx] > ts->max_score)
					ts->max_score = ts->block_max_scores[block_idx];
			}

			ts->max_score *= ts->query_freq;
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
 * Compare terms by current doc_id for initial sort.
 * Exhausted terms (UINT32_MAX) sort to the end.
 */
static int
compare_term_doc_id(const void *a, const void *b)
{
	const TpTermState *ta = (const TpTermState *)a;
	const TpTermState *tb = (const TpTermState *)b;
	uint32			   da = term_current_doc_id((TpTermState *)ta);
	uint32			   db = term_current_doc_id((TpTermState *)tb);

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
sort_terms_by_doc_id(TpTermState *terms, int term_count)
{
	qsort(terms, term_count, sizeof(TpTermState), compare_term_doc_id);
}

/*
 * Restore sorted order after term at position 'ord' advanced.
 * The term's doc_id increased, so it may need to move right.
 * Uses linear insertion -- O(1) typical, O(T) worst case.
 */
static void
restore_ordering(TpTermState *terms, int term_count, int ord)
{
	TpTermState tmp;
	uint32		doc_id = term_current_doc_id(&terms[ord]);
	int			i;

	for (i = ord + 1; i < term_count; i++)
	{
		if (term_current_doc_id(&terms[i]) >= doc_id)
			break;
	}

	/* Term needs to move from ord to i-1 */
	if (i > ord + 1)
	{
		tmp = terms[ord];
		memmove(&terms[ord],
				&terms[ord + 1],
				(i - ord - 1) * sizeof(TpTermState));
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
		TpTermState *terms,
		int			 term_count,
		float4		 threshold,
		int			*pivot_len_out,
		uint32		*pivot_doc_id_out)
{
	float4 accumulated = 0.0f;
	int	   i;

	for (i = 0; i < term_count; i++)
	{
		uint32 doc_id = term_current_doc_id(&terms[i]);
		if (doc_id == UINT32_MAX)
			break; /* No more active terms */

		accumulated += terms[i].max_score;
		if (accumulated > threshold)
		{
			/*
			 * Found pivot: terms[i] is the pivot term.
			 * Include any subsequent terms at the same doc_id.
			 */
			uint32 pivot_doc = doc_id;
			int	   pivot_len = i + 1;

			while (pivot_len < term_count &&
				   term_current_doc_id(&terms[pivot_len]) == pivot_doc)
				pivot_len++;

			*pivot_len_out	  = pivot_len;
			*pivot_doc_id_out = pivot_doc;
			return true;
		}
	}

	return false; /* Can't beat threshold */
}

/*
 * Compute block-max score upper bound at pivot.
 *
 * After WAND pivot selection using global max_scores, refine
 * with actual block-level upper bounds. Only considers terms
 * [0..pivot_len-1] since they're the only ones at or before
 * the pivot.
 */
static float4
compute_block_max_at_pivot(TpTermState *terms, int pivot_len)
{
	float4 upper_bound = 0.0f;
	int	   i;

	for (i = 0; i < pivot_len; i++)
	{
		TpTermState *ts = &terms[i];
		uint16		 block;

		if (!ts->found || ts->iter.finished)
			continue;
		if (ts->block_max_scores == NULL)
			continue;

		block = ts->iter.current_block;
		if (block < ts->iter.dict_entry.block_count)
			upper_bound += ts->block_max_scores[block] * ts->query_freq;
	}

	return upper_bound;
}

/*
 * When block-max upper bound < threshold, advance one scorer.
 *
 * Strategy (following Tantivy):
 * 1. Find the term with highest max_score among pivot terms
 * 2. Find the minimum last_doc_id of current blocks among
 *    pivot terms (next interesting boundary)
 * 3. Seek that term to min(boundary+1, first non-pivot doc)
 * 4. Restore sorted order
 */
static void
block_max_skip_advance(
		TpTermState *terms,
		int			 term_count,
		int			 pivot_len,
		int			*active_count,
		TpBMWStats	*stats)
{
	int	   best_scorer	  = -1;
	float4 best_max_score = -1.0f;
	uint32 min_block_end  = UINT32_MAX;
	uint32 seek_target;
	int	   i;

	/* Find scorer with highest max_score and minimum block end */
	for (i = 0; i < pivot_len; i++)
	{
		TpTermState *ts = &terms[i];
		uint32		 doc_id;
		uint16		 block;
		uint32		 block_last;

		doc_id = term_current_doc_id(ts);
		if (doc_id == UINT32_MAX)
			continue;

		if (terms[i].max_score > best_max_score)
		{
			best_max_score = terms[i].max_score;
			best_scorer	   = i;
		}

		block = ts->iter.current_block;
		if (ts->block_last_doc_ids != NULL &&
			block < ts->iter.dict_entry.block_count)
		{
			block_last = ts->block_last_doc_ids[block];
			if (block_last < min_block_end)
				min_block_end = block_last;
		}
	}

	if (best_scorer < 0)
		return;

	/* Seek target: past current blocks, capped by non-pivot */
	if (min_block_end == UINT32_MAX)
		seek_target = term_current_doc_id(&terms[best_scorer]) + 1;
	else
		seek_target = min_block_end + 1;

	/* Don't skip past the first non-pivot term */
	if (pivot_len < term_count)
	{
		uint32 next_doc = term_current_doc_id(&terms[pivot_len]);
		if (next_doc < seek_target)
			seek_target = next_doc;
	}

	/* Seek the best scorer */
	if (!seek_term_to_doc(&terms[best_scorer], seek_target))
		(*active_count)--;

	restore_ordering(terms, term_count, best_scorer);

	if (stats)
	{
		stats->blocks_skipped++;
		stats->seeks_performed++;
	}
}

/*
 * Seek all pre-pivot terms [0..pivot_len-1] to pivot_doc_id.
 *
 * Returns true if all pivot terms are aligned at pivot_doc_id.
 * Returns false if a term was exhausted or overshot, requiring
 * a re-pivot. Updates active_count on term exhaustion.
 */
static bool
seek_to_pivot(
		TpTermState *terms,
		int			 term_count,
		int			 pivot_len,
		uint32		 pivot_doc_id,
		int			*active_count)
{
	int i;

	for (i = 0; i < pivot_len; i++)
	{
		uint32 doc_id = term_current_doc_id(&terms[i]);

		if (doc_id == UINT32_MAX)
		{
			(*active_count)--;
			return false;
		}

		if (doc_id < pivot_doc_id)
		{
			if (!seek_term_to_doc(&terms[i], pivot_doc_id))
			{
				(*active_count)--;
				restore_ordering(terms, term_count, i);
				return false;
			}
			/*
			 * After seek, term may have moved past pivot.
			 * restore_ordering will reposition it correctly.
			 */
			restore_ordering(terms, term_count, i);
			/*
			 * A new term slid into position i, re-check it.
			 */
			i--;
			continue;
		}
	}

	return true;
}

/*
 * Verify all pivot terms are exactly at pivot_doc_id.
 *
 * After seeking, some terms may have overshot past the pivot.
 * Returns true only if all [0..pivot_len-1] are at pivot_doc_id.
 */
static bool
verify_pivot_alignment(TpTermState *terms, int pivot_len, uint32 pivot_doc_id)
{
	int i;

	for (i = 0; i < pivot_len; i++)
	{
		if (term_current_doc_id(&terms[i]) != pivot_doc_id)
			return false;
	}
	return true;
}

/*
 * Score a single document at pivot_doc_id across pivot terms.
 *
 * Only scores terms [0..pivot_len-1] because terms past pivot_len
 * have doc_id > pivot_doc_id by construction (seek_to_pivot only
 * advances pre-pivot terms; post-pivot terms stay put).
 */
static float4
score_pivot_document(
		TpTermState *terms,
		int			 pivot_len,
		float4		 k1,
		float4		 b,
		float4		 avg_doc_len)
{
	float4 doc_score = 0.0f;
	int	   i;

	for (i = 0; i < pivot_len; i++)
	{
		TpTermState	   *ts = &terms[i];
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
 * Block-max refinement then checks if block-level upper bounds
 * at the pivot still beat the threshold, and uses Tantivy-style
 * skip advancement when they don't.
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
		float4 block_upper;
		float4 doc_score;
		int	   i;

		threshold = tp_topk_threshold(heap);

		/* Step 1: Find WAND pivot */
		if (!find_wand_pivot(
					terms, term_count, threshold, &pivot_len, &pivot_doc_id))
			break; /* No term combination can beat threshold */

		/* Step 2: Seek pre-pivot terms to pivot_doc_id */
		if (!seek_to_pivot(
					terms, term_count, pivot_len, pivot_doc_id, &active_count))
			continue; /* Re-pivot with updated positions */

		/* Step 3: Block-max refinement */
		block_upper = compute_block_max_at_pivot(terms, pivot_len);

		if (block_upper <= threshold)
		{
			block_max_skip_advance(
					terms, term_count, pivot_len, &active_count, stats);
			continue;
		}

		if (stats)
			stats->blocks_scanned++;

		/* Step 4: Verify all pivot terms are at pivot_doc_id */
		if (!verify_pivot_alignment(terms, pivot_len, pivot_doc_id))
			continue; /* Re-pivot with new positions */

		/* Step 5: Score the pivot document */
		doc_score = score_pivot_document(terms, pivot_len, k1, b, avg_doc_len);

		if (doc_score > 0.0f && !tp_topk_dominated(heap, doc_score))
			tp_topk_add_segment(
					heap, reader->root_block, pivot_doc_id, doc_score);

		if (stats)
			stats->segment_docs_scored++;

		/*
		 * Step 6: Advance all pivot terms past pivot_doc_id.
		 * Iterate backward so restore_ordering shifts don't
		 * skip any terms.
		 */
		for (i = pivot_len - 1; i >= 0; i--)
		{
			if (term_current_doc_id(&terms[i]) == pivot_doc_id)
			{
				if (!advance_term_iterator(&terms[i]))
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

	/* Resolve CTIDs for segment results before extraction */
	tp_topk_resolve_ctids(&heap, index);

	/* Extract sorted results */
	result_count = tp_topk_extract(&heap, result_ctids, result_scores);

	if (stats)
		stats->docs_in_results = result_count;

	return result_count;
}
