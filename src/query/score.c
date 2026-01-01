/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * query/score.c - BM25 scoring operators and document ranking
 */
#include <postgres.h>

#include <math.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/memutils.h>

#include "memtable/memtable.h"
#include "memtable/source.h"
#include "memtable/stringtable.h"
#include "query/bmw.h"
#include "query/score.h"
#include "segment/segment.h"
#include "source.h"
#include "state/metapage.h"
#include "state/state.h"

/*
 * Centralized IDF calculation (basic version)
 * Calculates IDF using BM25 formula: log(1 + (N - df + 0.5) / (df + 0.5))
 * This formula ensures IDF is always non-negative since log(1 + x) >= 0
 * for all x >= 0.
 */
float4
tp_calculate_idf(int32 doc_freq, int32 total_docs)
{
	double idf_numerator   = (double)(total_docs - doc_freq + 0.5);
	double idf_denominator = (double)(doc_freq + 0.5);
	double idf_ratio	   = idf_numerator / idf_denominator;
	return (float4)log(1.0 + idf_ratio);
}

/*
 * Create and initialize hash table for document scores
 */
static HTAB *
tp_create_doc_scores_hash(int max_results, int32 total_docs)
{
	HASHCTL hash_ctl;
	int		initial_size;

	/*
	 * Size hash table reasonably - it will grow if needed. Avoid sizing for
	 * total_docs which could be tens of millions.
	 */
	initial_size = Max(max_results * 10, 1000);
	if (initial_size > total_docs)
		initial_size = total_docs;

	/* Initialize hash control structure */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(DocumentScoreEntry);

	/* Create hash table */
	return hash_create(
			"Document Scores",
			initial_size,
			&hash_ctl,
			HASH_ELEM | HASH_BLOBS);
}

/*
 * Inline comparison for document scores (descending order).
 * Returns true if a should come before b.
 * Primary: higher score first. Secondary: lower CTID first (for stability).
 */
static inline bool
doc_score_greater(const DocumentScoreEntry *a, const DocumentScoreEntry *b)
{
	if (a->score != b->score)
		return a->score > b->score;
	/* Tiebreaker: sort by CTID ascending for deterministic ordering */
	return ItemPointerCompare((ItemPointer)&a->ctid, (ItemPointer)&b->ctid) <
		   0;
}

#define DOC_SCORE_GREATER(a, b) doc_score_greater((a), (b))

/*
 * Swap two document score pointers.
 */
static inline void
swap_doc_ptrs(DocumentScoreEntry **a, DocumentScoreEntry **b)
{
	DocumentScoreEntry *tmp = *a;
	*a						= *b;
	*b						= tmp;
}

/*
 * Insertion sort for small arrays - used for final sorting of top-k.
 * Inlined comparison for performance.
 */
static void
insertion_sort_docs(DocumentScoreEntry **arr, int n)
{
	for (int i = 1; i < n; i++)
	{
		DocumentScoreEntry *key = arr[i];
		int					j	= i - 1;

		/* Move elements that key should come before (descending score, then
		 * CTID) */
		while (j >= 0 && DOC_SCORE_GREATER(key, arr[j]))
		{
			arr[j + 1] = arr[j];
			j--;
		}
		arr[j + 1] = key;
	}
}

/*
 * Partition for quickselect - returns pivot index.
 * Partitions so elements with higher scores are on the left.
 * Uses median-of-three pivot selection and inlined comparison.
 */
static int
partition_docs(DocumentScoreEntry **arr, int left, int right)
{
	int					mid = left + (right - left) / 2;
	DocumentScoreEntry *pivot;
	int					store_idx;

	/* Median-of-three pivot selection */
	if (DOC_SCORE_GREATER(arr[mid], arr[left]))
		swap_doc_ptrs(&arr[left], &arr[mid]);
	if (DOC_SCORE_GREATER(arr[right], arr[left]))
		swap_doc_ptrs(&arr[left], &arr[right]);
	if (DOC_SCORE_GREATER(arr[mid], arr[right]))
		swap_doc_ptrs(&arr[mid], &arr[right]);

	/* Pivot is now at arr[right] */
	pivot	  = arr[right];
	store_idx = left;

	for (int i = left; i < right; i++)
	{
		/* Higher scores go to the left (descending order) */
		if (DOC_SCORE_GREATER(arr[i], pivot))
		{
			swap_doc_ptrs(&arr[store_idx], &arr[i]);
			store_idx++;
		}
	}
	swap_doc_ptrs(&arr[store_idx], &arr[right]);
	return store_idx;
}

/*
 * Partial quicksort: ensures top-k elements are sorted in positions [0, k).
 * Uses quickselect to partition, then only recurses into relevant partitions.
 * O(n + k log k) average case instead of O(n log n) for full sort.
 */
static void
partial_quicksort_docs(DocumentScoreEntry **arr, int left, int right, int k)
{
	int pivot_idx;

	while (left < right)
	{
		/* Use insertion sort for small subarrays */
		if (right - left < 16)
		{
			insertion_sort_docs(arr + left, right - left + 1);
			return;
		}

		pivot_idx = partition_docs(arr, left, right);

		/*
		 * We need elements [0, k) sorted.
		 * - If pivot_idx < k: left side is done, need to sort right side
		 *   up to position k-1
		 * - If pivot_idx >= k: only need to sort left side
		 * - If pivot_idx == k-1: left side needs sorting, right side doesn't
		 */
		if (pivot_idx >= k)
		{
			/* Only recurse left - right side doesn't matter */
			right = pivot_idx - 1;
		}
		else
		{
			/* Left side might need sorting, recurse into it */
			if (pivot_idx > left)
				partial_quicksort_docs(arr, left, pivot_idx - 1, k);
			/* Continue with right side (tail recursion elimination) */
			left = pivot_idx + 1;
		}
	}
}

/*
 * Sort only the top-k elements of an array by score (descending).
 * After this call, arr[0..k-1] contain the k highest-scoring elements, sorted.
 * Elements at arr[k..n-1] are in undefined order but all have lower scores.
 */
static void
sort_top_k_docs(DocumentScoreEntry **arr, int n, int k)
{
	if (n <= 1 || k <= 0)
		return;

	if (k >= n)
		k = n;

	partial_quicksort_docs(arr, 0, n - 1, k);
}

/*
 * Extract and sort documents by BM25 score (descending)
 */
static DocumentScoreEntry **
tp_extract_and_sort_documents(
		HTAB *doc_scores_hash, int max_results, int *actual_count)
{
	DocumentScoreEntry **all_docs;
	DocumentScoreEntry **sorted_docs;
	int					 total_count;
	int					 result_count;
	int					 i;

	/* Get the total number of documents */
	total_count = hash_get_num_entries(doc_scores_hash);

	if (total_count == 0)
	{
		*actual_count = 0;
		return NULL;
	}

	/* Allocate array for ALL documents first */
	all_docs = (DocumentScoreEntry **)palloc(
			total_count * sizeof(DocumentScoreEntry *));

	/* Extract ALL documents from hash table */
	{
		HASH_SEQ_STATUS		seq_status;
		DocumentScoreEntry *current_entry;

		hash_seq_init(&seq_status, doc_scores_hash);
		i = 0;

		while ((current_entry = (DocumentScoreEntry *)hash_seq_search(
						&seq_status)) != NULL)
		{
			all_docs[i++] = current_entry;
		}

		Assert(i == total_count);
	}

	/* Determine how many results we need */
	result_count = (total_count < max_results) ? total_count : max_results;

	/*
	 * Partial sort: only sort the top result_count elements.
	 * O(n + k log k) instead of O(n log n) for full qsort.
	 */
	sort_top_k_docs(all_docs, total_count, result_count);

	if (result_count < total_count)
	{
		/* Need to copy just the top results to a smaller array */
		sorted_docs = (DocumentScoreEntry **)palloc(
				result_count * sizeof(DocumentScoreEntry *));
		for (i = 0; i < result_count; i++)
			sorted_docs[i] = all_docs[i];
		pfree(all_docs);
	}
	else
	{
		/* Use the full array */
		sorted_docs = all_docs;
	}

	*actual_count = result_count;
	return sorted_docs;
}

/*
 * Get unified doc_freq for a term (memtable + all segments).
 * Returns 0 if term not found in any source.
 */
static uint32
tp_get_unified_doc_freq(
		TpLocalIndexState *local_state,
		Relation		   index,
		const char		  *term,
		BlockNumber		  *level_heads)
{
	uint32		   doc_freq = 0;
	TpPostingList *posting_list;
	int			   level;

	/* Get doc_freq from memtable */
	posting_list = tp_get_posting_list(local_state, term);
	if (posting_list && posting_list->doc_count > 0)
		doc_freq = posting_list->doc_count;

	/* Add doc_freq from all segment levels */
	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		if (level_heads[level] != InvalidBlockNumber)
		{
			doc_freq +=
					tp_segment_get_doc_freq(index, level_heads[level], term);
		}
	}

	return doc_freq;
}

/*
 * Copy results to output arrays
 */
static void
tp_copy_results_to_output(
		DocumentScoreEntry **sorted_docs,
		int					 scored_count,
		ItemPointer			 additional_ctids,
		int					 additional_count,
		ItemPointer			 result_ctids,
		float4			   **result_scores)
{
	int		total_results = scored_count + additional_count;
	float4 *scores;
	int		i;

	/* Allocate scores array */
	scores = (float4 *)palloc(total_results * sizeof(float4));

	/* Copy scored documents */
	for (i = 0; i < scored_count; i++)
	{
		result_ctids[i] = sorted_docs[i]->ctid;
		/* Store BM25 score as computed (should be positive) */
		scores[i] = sorted_docs[i]->score;
	}

	/* Copy additional zero-scored documents */
	for (i = 0; i < additional_count; i++)
	{
		result_ctids[scored_count + i] = additional_ctids[i];
		scores[scored_count + i]	   = 0.0f; /* Zero score */
	}

	*result_scores = scores;
}

/*
 * Score documents using BM25 algorithm
 * Returns number of documents scored
 */
int
tp_score_documents(
		TpLocalIndexState *local_state,
		Relation		   index_relation,
		char			 **query_terms,
		int32			  *query_frequencies,
		int				   query_term_count,
		float4			   k1,
		float4			   b,
		int				   max_results,
		ItemPointer		   result_ctids,
		float4			 **result_scores)
{
	HTAB				*doc_scores_hash;
	DocumentScoreEntry **sorted_docs;
	float4				 avg_doc_len;
	int32				 total_docs;
	int					 scored_count = 0;
	TpDataSource		*memtable_source;
	TpIndexMetaPage		 metap;
	BlockNumber			 level_heads[TP_MAX_LEVELS];
	uint32				*unified_doc_freqs;
	int					 i;

	/* Basic sanity checks */
	Assert(local_state != NULL);
	Assert(query_terms != NULL);
	Assert(result_ctids != NULL);
	Assert(result_scores != NULL);

	if (query_term_count <= 0 || max_results <= 0)
		return 0;

	if (!local_state->shared)
	{
		elog(ERROR, "tp_score_documents: shared state is NULL!");
		return 0;
	}

	total_docs	= local_state->shared->total_docs;
	avg_doc_len = total_docs > 0 ? (float4)(local_state->shared->total_len /
											(double)total_docs)
								 : 0.0f;

	if (total_docs <= 0)
		return 0;

	/* Get segment level heads for querying all levels */
	metap = tp_get_metapage(index_relation);
	for (i = 0; i < TP_MAX_LEVELS; i++)
		level_heads[i] = metap->level_heads[i];
	pfree(metap);

	/* If avg_doc_len is 0, all documents have zero length and
	 * would get zero BM25 scores */
	if (avg_doc_len <= 0.0f)
		return 0;

	/*
	 * BMW fast path for single-term queries.
	 * Uses Block-Max WAND to skip blocks that can't contribute to top-k.
	 */
	if (query_term_count == 1)
	{
		const char *term = query_terms[0];
		uint32		doc_freq;
		float4		idf;
		float4	   *scores;
		int			result_count;

		/* Get unified doc_freq across memtable and segments */
		doc_freq = tp_get_unified_doc_freq(
				local_state, index_relation, term, level_heads);
		if (doc_freq == 0)
			return 0;

		/* Calculate IDF */
		idf = tp_calculate_idf(doc_freq, total_docs);

		/* Allocate scores array */
		scores = (float4 *)palloc(max_results * sizeof(float4));

		/* Run BMW scoring */
		result_count = tp_score_single_term_bmw(
				local_state,
				index_relation,
				term,
				idf,
				k1,
				b,
				avg_doc_len,
				max_results,
				result_ctids,
				scores,
				NULL);

		*result_scores = scores;
		return result_count;
	}

	/*
	 * BMW fast path for multi-term queries (2-8 terms).
	 * Uses block-level upper bounds to skip non-contributing blocks.
	 */
	if (query_term_count <= 8)
	{
		float4 *idfs;
		float4 *scores;
		int		result_count;
		int		i;

		/* Compute unified doc_freqs and IDFs for all terms */
		idfs = palloc(query_term_count * sizeof(float4));
		for (i = 0; i < query_term_count; i++)
		{
			uint32 doc_freq = tp_get_unified_doc_freq(
					local_state, index_relation, query_terms[i], level_heads);
			idfs[i] = (doc_freq > 0) ? tp_calculate_idf(doc_freq, total_docs)
									 : 0.0f;
		}

		/* Allocate scores array */
		scores = (float4 *)palloc(max_results * sizeof(float4));

		/* Run multi-term BMW scoring */
		result_count = tp_score_multi_term_bmw(
				local_state,
				index_relation,
				query_terms,
				query_term_count,
				query_frequencies,
				idfs,
				k1,
				b,
				avg_doc_len,
				max_results,
				result_ctids,
				scores,
				NULL);

		pfree(idfs);

		/* If BMW returns -1, fall through to exhaustive path */
		if (result_count >= 0)
		{
			*result_scores = scores;
			return result_count;
		}
		pfree(scores);
	}

	/* Create hash table for accumulating document scores - needed for both
	 * memtable and segment searches */
	doc_scores_hash = tp_create_doc_scores_hash(max_results, total_docs);
	if (!doc_scores_hash)
		elog(ERROR, "Failed to create document scores hash table");

	/* Allocate array for unified doc_freqs across all sources */
	unified_doc_freqs = palloc0(query_term_count * sizeof(uint32));

	/* Create memtable data source and score documents */
	memtable_source = tp_memtable_source_create(local_state);
	if (memtable_source)
	{
		/* Score documents from memtable using the data source interface */
		for (int term_idx = 0; term_idx < query_term_count; term_idx++)
		{
			const char	  *term = query_terms[term_idx];
			TpPostingData *postings;
			float4		   idf;

			postings = tp_source_get_postings(memtable_source, term);
			if (!postings || postings->count == 0)
			{
				if (postings)
					tp_source_free_postings(memtable_source, postings);
				continue;
			}

			/* Update unified doc_freq from this source */
			unified_doc_freqs[term_idx] = postings->doc_freq;

			/* Calculate IDF using memtable doc_freq */
			idf = tp_calculate_idf(unified_doc_freqs[term_idx], total_docs);

			/* Process each posting in columnar format */
			for (int i = 0; i < postings->count; i++)
			{
				ItemPointerData	   *ctid = &postings->ctids[i];
				float4				tf	 = (float4)postings->frequencies[i];
				float4				doc_len;
				float4				term_score;
				double				numerator_d, denominator_d;
				DocumentScoreEntry *doc_entry;
				bool				found;
				int32				doc_len_int;

				/* Validate TID first */
				if (!ItemPointerIsValid(ctid))
					continue;

				/* Get document length from source */
				doc_len_int = tp_source_get_doc_length(memtable_source, ctid);
				if (doc_len_int <= 0)
					continue;
				doc_len = (float4)doc_len_int;

				/* Calculate BM25 term score contribution */
				numerator_d = (double)tf * ((double)k1 + 1.0);

				/* Standard BM25 denominator calculation */
				denominator_d = (double)tf +
								(double)k1 *
										(1.0 - (double)b +
										 (double)b * ((double)doc_len /
													  (double)avg_doc_len));

				term_score = (float4)((double)idf *
									  (numerator_d / denominator_d) *
									  (double)query_frequencies[term_idx]);

				/* Find or create document entry in hash table */
				doc_entry = (DocumentScoreEntry *)
						hash_search(doc_scores_hash, ctid, HASH_ENTER, &found);

				if (!found)
				{
					/* New document - initialize */
					doc_entry->ctid		  = *ctid;
					doc_entry->score	  = term_score;
					doc_entry->doc_length = doc_len;
				}
				else
				{
					/* Existing document - accumulate score */
					doc_entry->score += term_score;
				}
			}

			tp_source_free_postings(memtable_source, postings);
		}

		tp_source_close(memtable_source);
	}

	/*
	 * Score documents from all segment levels efficiently.
	 * This opens each segment ONCE and processes all terms, instead of
	 * opening each segment once per term (which was O(terms * segments)).
	 */
	for (int level = 0; level < TP_MAX_LEVELS; level++)
	{
		if (level_heads[level] == InvalidBlockNumber)
			continue;

		tp_score_all_terms_in_segment_chain(
				index_relation,
				level_heads[level],
				query_terms,
				query_term_count,
				query_frequencies,
				unified_doc_freqs, /* doc_freqs accumulated here */
				total_docs,
				k1,
				b,
				avg_doc_len,
				doc_scores_hash);
	}

	/* Free unified doc_freqs array */
	pfree(unified_doc_freqs);

	/* Extract and sort documents by score */
	sorted_docs = tp_extract_and_sort_documents(
			doc_scores_hash, max_results, &scored_count);

	/* Copy results to output arrays (no additional zero-scored documents) */
	tp_copy_results_to_output(
			sorted_docs,
			scored_count,
			NULL, /* no additional_ctids */
			0,	  /* no additional_count */
			result_ctids,
			result_scores);

	/* Cleanup */
	if (sorted_docs)
		pfree(sorted_docs);
	hash_destroy(doc_scores_hash);

	return scored_count;
}
