/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * operator.c - BM25 scoring operators
 */
#include <postgres.h>

#include <math.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/memutils.h>

#include "doc_scores.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "metapage.h"
#include "operator.h"
#include "segment/segment.h"
#include "state.h"

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
 * Create document score table with reasonable initial size.
 */
static TpDocScoreTable *
tp_create_doc_scores_table(int max_results, int32 total_docs)
{
	int initial_size;

	/*
	 * Size table reasonably - it will grow if needed. Avoid sizing for
	 * total_docs which could be tens of millions.
	 */
	initial_size = Max(max_results * 10, 1000);
	if (initial_size > total_docs)
		initial_size = total_docs;

	return tp_doc_score_table_create(initial_size, CurrentMemoryContext);
}

/*
 * Inline comparison for document scores (descending order).
 * Returns true if a should come before b.
 * Primary: higher score first. Secondary: lower key (CTID) first for
 * stability.
 */
static inline bool
doc_score_greater(const TpDocScoreEntry *a, const TpDocScoreEntry *b)
{
	if (a->score != b->score)
		return a->score > b->score;
	/* Tiebreaker: sort by key (packed CTID) ascending for deterministic order
	 */
	return a->key < b->key;
}

#define DOC_SCORE_GREATER(a, b) doc_score_greater((a), (b))

/*
 * Swap two document score pointers.
 */
static inline void
swap_doc_ptrs(TpDocScoreEntry **a, TpDocScoreEntry **b)
{
	TpDocScoreEntry *tmp = *a;
	*a					 = *b;
	*b					 = tmp;
}

/*
 * Insertion sort for small arrays - used for final sorting of top-k.
 * Inlined comparison for performance.
 */
static void
insertion_sort_docs(TpDocScoreEntry **arr, int n)
{
	for (int i = 1; i < n; i++)
	{
		TpDocScoreEntry *key = arr[i];
		int				 j	 = i - 1;

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
partition_docs(TpDocScoreEntry **arr, int left, int right)
{
	int				 mid = left + (right - left) / 2;
	TpDocScoreEntry *pivot;
	int				 store_idx;

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
partial_quicksort_docs(TpDocScoreEntry **arr, int left, int right, int k)
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
sort_top_k_docs(TpDocScoreEntry **arr, int n, int k)
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
static TpDocScoreEntry **
tp_extract_and_sort_documents(
		TpDocScoreTable *doc_scores, int max_results, int *actual_count)
{
	TpDocScoreEntry	 **all_docs;
	TpDocScoreEntry	 **sorted_docs;
	int				   total_count;
	int				   result_count;
	int				   i;
	TpDocScoreIterator iter;
	TpDocScoreEntry	  *current_entry;

	/* Get the total number of documents */
	total_count = tp_doc_score_table_count(doc_scores);

	if (total_count == 0)
	{
		*actual_count = 0;
		return NULL;
	}

	/* Allocate array for ALL documents first */
	all_docs = (TpDocScoreEntry **)palloc(
			total_count * sizeof(TpDocScoreEntry *));

	/* Extract ALL documents from hash table */
	tp_doc_score_iter_init(&iter, doc_scores);
	i = 0;

	while ((current_entry = tp_doc_score_iter_next(&iter)) != NULL)
	{
		all_docs[i++] = current_entry;
	}

	Assert(i == total_count);

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
		sorted_docs = (TpDocScoreEntry **)palloc(
				result_count * sizeof(TpDocScoreEntry *));
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
 * Copy results to output arrays
 */
static void
tp_copy_results_to_output(
		TpDocScoreEntry **sorted_docs,
		int				  scored_count,
		ItemPointer		  additional_ctids,
		int				  additional_count,
		ItemPointer		  result_ctids,
		float4			**result_scores)
{
	int		total_results = scored_count + additional_count;
	float4 *scores;
	int		i;

	/* Allocate scores array */
	scores = (float4 *)palloc(total_results * sizeof(float4));

	/* Copy scored documents */
	for (i = 0; i < scored_count; i++)
	{
		/* Convert packed key back to CTID */
		key_to_ctid(sorted_docs[i]->key, &result_ctids[i]);
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
	TpDocScoreTable	 *doc_scores;
	TpDocScoreEntry **sorted_docs;
	float4			  avg_doc_len;
	int32			  total_docs;
	int				  scored_count = 0;
	dshash_table	 *string_table;
	dshash_table	 *doclength_table = NULL;
	TpMemtable		 *memtable;
	TpIndexMetaPage	  metap;
	BlockNumber		  level_heads[TP_MAX_LEVELS];
	uint32			 *unified_doc_freqs;
	int				  i;
	int				  term_idx;

	/* Basic sanity checks */
	Assert(local_state != NULL);
	Assert(query_terms != NULL);
	Assert(result_ctids != NULL);
	Assert(result_scores != NULL);

	if (query_term_count <= 0 || max_results <= 0)
		return 0;

	/* Get memtable and corpus statistics */
	memtable = get_memtable(local_state);
	if (!memtable)
	{
		elog(ERROR, "Cannot get memtable for scoring - index state corrupted");
		return 0; /* Never reached */
	}

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

	/* Create hash table for accumulating document scores - needed for both
	 * memtable and segment searches */
	doc_scores = tp_create_doc_scores_table(max_results, total_docs);
	if (!doc_scores)
		elog(ERROR, "Failed to create document scores table");

	/* Get string hash table if available - may be empty after spill */
	if (memtable->string_hash_handle != DSHASH_HANDLE_INVALID)
	{
		string_table = tp_string_table_attach(
				local_state->dsa, memtable->string_hash_handle);
	}
	else
	{
		string_table = NULL;
	}

	/* Attach to document length table for memtable scoring */
	if (memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		doclength_table = tp_doclength_table_attach(
				local_state->dsa, memtable->doc_lengths_handle);
	}

	/* Get doc_freq from memtable for all terms */
	unified_doc_freqs = palloc0(query_term_count * sizeof(uint32));
	if (string_table)
	{
		for (term_idx = 0; term_idx < query_term_count; term_idx++)
		{
			TpPostingList *posting_list = tp_string_table_get_posting_list(
					local_state->dsa, string_table, query_terms[term_idx]);
			if (posting_list && posting_list->doc_count > 0)
				unified_doc_freqs[term_idx] = posting_list->doc_count;
		}
	}

	/* Calculate BM25 scores from memtable if available */
	if (string_table)
	{
		for (int term_idx = 0; term_idx < query_term_count; term_idx++)
		{
			const char	   *term = query_terms[term_idx];
			TpPostingList  *posting_list;
			TpPostingEntry *entries;
			float4			idf;

			posting_list = tp_string_table_get_posting_list(
					local_state->dsa, string_table, term);

			if (!posting_list || posting_list->doc_count == 0)
				continue;

			/* Calculate IDF using unified doc_freq from all sources */
			idf = tp_calculate_idf(unified_doc_freqs[term_idx], total_docs);

			/* Get posting entries */
			entries = tp_get_posting_entries(local_state->dsa, posting_list);
			if (!entries)
				continue;

			/* Process each document in this term's posting list */
			for (int doc_idx = 0; doc_idx < posting_list->doc_count; doc_idx++)
			{
				TpPostingEntry	*entry = &entries[doc_idx];
				float4			 tf	   = entry->frequency;
				float4			 doc_len;
				float4			 term_score;
				double			 numerator_d, denominator_d;
				TpDocScoreEntry *doc_entry;
				bool			 found;

				/* Validate TID first */
				if (!ItemPointerIsValid(&entry->ctid))
					continue;

				/* Look up document length from hash table */
				if (!doclength_table)
				{
					elog(ERROR,
						 "Document length table not available for scoring");
				}
				{
					int32 doc_len_int = tp_get_document_length_attached(
							doclength_table, &entry->ctid);
					if (doc_len_int <= 0)
					{
						elog(ERROR,
							 "Failed to get document length for ctid (%u,%u)",
							 BlockIdGetBlockNumber(&entry->ctid.ip_blkid),
							 entry->ctid.ip_posid);
					}
					doc_len = (float4)doc_len_int;
				}

				/* Calculate BM25 term score contribution */
				numerator_d = (double)tf * ((double)k1 + 1.0);

				/* Standard BM25 denominator calculation - avg_doc_len > 0 is
				 * guaranteed */
				denominator_d = (double)tf +
								(double)k1 *
										(1.0 - (double)b +
										 (double)b * ((double)doc_len /
													  (double)avg_doc_len));

				term_score = (float4)((double)idf *
									  (numerator_d / denominator_d) *
									  (double)query_frequencies[term_idx]);

				/* Find or create document entry */
				doc_entry = tp_doc_score_table_insert(
						doc_scores, &entry->ctid, &found);

				if (!found)
				{
					/* New document - initialize */
					doc_entry->score	  = term_score;
					doc_entry->doc_length = doc_len;
				}
				else
				{
					/* Existing document - accumulate score */
					doc_entry->score += term_score;
				}
			}
		}

		dshash_detach(string_table);
	}

	/* Detach from document length table after memtable scoring */
	if (doclength_table)
		dshash_detach(doclength_table);

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
				doc_scores);
	}

	/* Free unified doc_freqs array */
	pfree(unified_doc_freqs);

	/* Extract and sort documents by score */
	sorted_docs = tp_extract_and_sort_documents(
			doc_scores, max_results, &scored_count);

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
	tp_doc_score_table_destroy(doc_scores);

	return scored_count;
}
