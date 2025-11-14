/*-------------------------------------------------------------------------
 *
 * operator.c
 *	  BM25 scoring and query operations for Tapir
 *
 * This module contains the top-level scoring functions that coordinate
 * between memtable, stringtable, and posting list components.
 *
 * IDENTIFICATION
 *	  src/operator.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <math.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/memutils.h>

#include "memtable.h"
#include "metapage.h"
#include "operator.h"
#include "posting.h"
#include "state.h"
#include "stringtable.h"

/*
 * Centralized IDF calculation (basic version)
 * Calculates IDF using a modified BM25 formula: log(1 + (N - df + 0.5) / (df +
 * 0.5))
 * This formula ensures IDF is always non-negative by adding 1 to the ratio
 * before taking the logarithm, preventing negative values that can occur with
 * very common terms.
 */
float4
tp_calculate_idf(int32 doc_freq, int32 total_docs)
{
	double idf_numerator   = (double)(total_docs - doc_freq + 0.5);
	double idf_denominator = (double)(doc_freq + 0.5);
	double idf_ratio	   = idf_numerator / idf_denominator;
	double raw_idf		   = log(1.0 + idf_ratio);

	/*
	 * With the modified formula (log(1 + ratio)), IDF is always non-negative
	 * since log(1 + x) >= 0 for all x >= 0, and our ratio is always >= 0.
	 */
	return (float4)raw_idf;
}

/*
 * IDF calculation with average IDF parameter
 * Uses modified BM25 formula: log(1 + (N - df + 0.5) / (df + 0.5))
 * The average_idf parameter is kept for compatibility but no longer needed
 * since this formula ensures non-negative IDF values.
 */
float4
tp_calculate_idf_with_epsilon(
		int32 doc_freq, int32 total_docs, float8 average_idf)
{
	double idf_numerator   = (double)(total_docs - doc_freq + 0.5);
	double idf_denominator = (double)(doc_freq + 0.5);
	double idf_ratio	   = idf_numerator / idf_denominator;
	double raw_idf		   = log(1.0 + idf_ratio);

	/*
	 * With the modified formula, IDF is always non-negative.
	 * No epsilon handling needed as log(1 + x) >= 0 for all x >= 0.
	 */
	return (float4)raw_idf;
}

/*
 * Document score entry for BM25 calculation
 */
typedef struct DocumentScoreEntry
{
	ItemPointerData ctid;		/* Document CTID (hash key) */
	float4			score;		/* Accumulated BM25 score */
	float4			doc_length; /* Document length (for BM25 calculation) */
} DocumentScoreEntry;

/*
 * Create and initialize hash table for document scores
 */
static HTAB *
tp_create_doc_scores_hash(int max_results, int32 total_docs)
{
	HASHCTL hash_ctl;

	/* Initialize hash control structure */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(DocumentScoreEntry);

	/* Create hash table */
	return hash_create(
			"Document Scores", total_docs, &hash_ctl, HASH_ELEM | HASH_BLOBS);
}

/*
 * Comparison function for qsort - sort by BM25 score descending (higher scores
 * first)
 */
static int
compare_document_scores(const void *a, const void *b)
{
	const DocumentScoreEntry *doc_a = *(const DocumentScoreEntry **)a;
	const DocumentScoreEntry *doc_b = *(const DocumentScoreEntry **)b;

	if (doc_a->score > doc_b->score)
		return -1; /* a comes before b (descending order) */
	else if (doc_a->score < doc_b->score)
		return 1; /* b comes before a */
	else
		return 0; /* equal scores */
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

	/* Sort ALL documents by score using qsort */
	qsort((void *)all_docs,
		  total_count,
		  sizeof(DocumentScoreEntry *),
		  compare_document_scores);

	/* Take only the top max_results */
	result_count = (total_count < max_results) ? total_count : max_results;

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
 * Scan docid pages to find all documents in the index
 * Returns an array of CTIDs and the count
 *
 * TODO: Refactor to use a callback function instead of allocating
 * and returning an array. This would avoid memory allocation and
 * allow the caller to process CTIDs as they're discovered.
 */
static ItemPointer
tp_scan_docid_pages(Relation index_relation, int *total_ctids)
{
	Buffer			   metabuf, docid_buf;
	Page			   metapage, docid_page;
	TpIndexMetaPage	   metap;
	TpDocidPageHeader *docid_header;
	ItemPointer		   docids, all_ctids;
	BlockNumber		   current_page;
	int				   capacity = 100; /* Initial capacity */
	int				   count	= 0;

	/* Initialize output */
	*total_ctids = 0;

	/* Allocate initial array */
	all_ctids = (ItemPointer)palloc(capacity * sizeof(ItemPointerData));

	/* Get the metapage to find the first docid page */
	metabuf = ReadBuffer(index_relation, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	current_page = metap->first_docid_page;
	UnlockReleaseBuffer(metabuf);

	if (current_page == InvalidBlockNumber)
	{
		pfree(all_ctids);
		return NULL;
	}

	/* Iterate through all docid pages */
	while (current_page != InvalidBlockNumber)
	{
		docid_buf = ReadBuffer(index_relation, current_page);
		LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
		docid_page	 = BufferGetPage(docid_buf);
		docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);

		/* Validate page magic */
		if (docid_header->magic != TP_DOCID_PAGE_MAGIC)
		{
			elog(WARNING,
				 "Invalid docid page magic on block %u, stopping scan",
				 current_page);
			UnlockReleaseBuffer(docid_buf);
			break;
		}

		/* Get docids from this page */
		docids = (ItemPointer)((char *)docid_header +
							   MAXALIGN(sizeof(TpDocidPageHeader)));

		/* Expand array if needed */
		if (count + docid_header->num_docids > capacity)
		{
			capacity  = count + docid_header->num_docids + 100;
			all_ctids = (ItemPointer)
					repalloc(all_ctids, capacity * sizeof(ItemPointerData));
		}

		/* Copy docids from this page */
		for (int i = 0; i < docid_header->num_docids; i++)
		{
			all_ctids[count++] = docids[i];
		}

		/* Move to next page */
		current_page = docid_header->next_page;
		UnlockReleaseBuffer(docid_buf);
	}

	*total_ctids = count;
	return all_ctids;
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
	int					 scored_count	  = 0;
	int					 additional_count = 0;
	int					 total_results;
	ItemPointer			 additional_ctids = NULL;
	dshash_table		*string_table;
	TpMemtable			*memtable;

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

	total_docs	= local_state->shared->total_docs;
	avg_doc_len = total_docs > 0 ? (float4)(local_state->shared->total_len /
											(double)total_docs)
								 : 0.0f;

	if (total_docs <= 0)
		return 0;

	/* If avg_doc_len is 0, all documents have zero length and
	 * would get zero BM25 scores */
	if (avg_doc_len <= 0.0f)
		return 0;

	/* Get string hash table */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
		return 0;

	string_table = tp_string_table_attach(
			local_state->dsa, memtable->string_hash_handle);
	if (!string_table)
		return 0;

	/* Create hash table for accumulating document scores */
	doc_scores_hash = tp_create_doc_scores_hash(max_results, total_docs);
	if (!doc_scores_hash)
	{
		dshash_detach(string_table);
		elog(ERROR, "Failed to create document scores hash table");
	}

	/* Calculate BM25 scores for matching documents */
	for (int term_idx = 0; term_idx < query_term_count; term_idx++)
	{
		const char	   *term = query_terms[term_idx];
		TpPostingList  *posting_list;
		TpPostingEntry *entries;
		float4			idf;
		float8			avg_idf;

		posting_list = tp_string_table_get_posting_list(
				local_state->dsa, string_table, term);

		if (!posting_list || posting_list->doc_count == 0)
			continue;

		/* Calculate IDF with epsilon handling */
		Assert(memtable->total_terms >
			   0); /* Must have terms if we have docs */
		avg_idf = local_state->shared->idf_sum / memtable->total_terms;
		idf		= tp_calculate_idf_with_epsilon(
				posting_list->doc_count, total_docs, avg_idf);

		/* Get posting entries */
		entries = tp_get_posting_entries(local_state->dsa, posting_list);
		if (!entries)
			continue;

		/* Process each document in this term's posting list */
		for (int doc_idx = 0; doc_idx < posting_list->doc_count; doc_idx++)
		{
			TpPostingEntry	   *entry = &entries[doc_idx];
			float4				tf	  = entry->frequency;
			float4				doc_len;
			float4				term_score;
			double				numerator_d, denominator_d;
			DocumentScoreEntry *doc_entry;
			bool				found;

			/* Look up document length from memtable hash table */
			doc_len = (float4)
					tp_get_document_length(local_state, &entry->ctid);
			if (doc_len <= 0.0f)
			{
				elog(ERROR,
					 "Failed to get document length for ctid (%u,%u)",
					 BlockIdGetBlockNumber(&entry->ctid.ip_blkid),
					 entry->ctid.ip_posid);
			}

			/* Validate TID */
			if (!ItemPointerIsValid(&entry->ctid))
				continue;

			/* Calculate BM25 term score contribution */
			numerator_d = (double)tf * ((double)k1 + 1.0);

			/* Standard BM25 denominator calculation - avg_doc_len > 0 is
			 * guaranteed */
			denominator_d = (double)tf +
							(double)k1 * (1.0 - (double)b +
										  (double)b * ((double)doc_len /
													   (double)avg_doc_len));

			term_score = (float4)((double)idf * (numerator_d / denominator_d) *
								  (double)query_frequencies[term_idx]);

			/* Find or create document entry in hash table */
			doc_entry = (DocumentScoreEntry *)hash_search(
					doc_scores_hash, &entry->ctid, HASH_ENTER, &found);

			if (!found)
			{
				/* New document - initialize */
				doc_entry->ctid	 = entry->ctid;
				doc_entry->score = term_score; /* BM25 score (can be positive
												  or negative) */
				doc_entry->doc_length = doc_len;
			}
			else
			{
				/* Existing document - accumulate score */
				float4 old_score = doc_entry->score;
				doc_entry->score += term_score; /* Accumulate BM25 score */
			}
		}
	}

	/* If we don't have enough scored results, find additional zero-scored
	 * documents */
	if (scored_count < max_results)
	{
		int			needed_additional = max_results - scored_count;
		int			found_additional  = 0;
		ItemPointer all_index_ctids;
		int			total_index_ctids;

		/* Allocate array for additional CTIDs */
		additional_ctids = palloc(needed_additional * sizeof(ItemPointerData));

		/* Scan docid pages to get all documents in the index */
		all_index_ctids =
				tp_scan_docid_pages(index_relation, &total_index_ctids);

		if (all_index_ctids && total_index_ctids > 0)
		{
			/* Check each document to see if it was already scored */
			for (int i = 0;
				 i < total_index_ctids && found_additional < needed_additional;
				 i++)
			{
				ItemPointer test_ctid = &all_index_ctids[i];

				/* Check if this CTID was already scored */
				DocumentScoreEntry *existing = (DocumentScoreEntry *)
						hash_search(
								doc_scores_hash, test_ctid, HASH_FIND, NULL);

				if (!existing)
				{
					/* Not scored - add as zero-scored document */
					additional_ctids[found_additional] = *test_ctid;
					found_additional++;
				}
			}

			pfree(all_index_ctids);
		}

		additional_count = found_additional;
	}

	/* Extract and sort documents by score */
	sorted_docs = tp_extract_and_sort_documents(
			doc_scores_hash, max_results, &scored_count);

	/* Ensure we don't exceed max_results total documents */
	total_results = scored_count + additional_count;
	if (total_results > max_results)
	{
		additional_count = max_results - scored_count;
		if (additional_count < 0)
			additional_count = 0;
	}

	/* Copy results to output arrays */
	tp_copy_results_to_output(
			sorted_docs,
			scored_count,
			additional_ctids,
			additional_count,
			result_ctids,
			result_scores);

	/* Cleanup */
	if (sorted_docs)
		pfree(sorted_docs);
	if (additional_ctids)
		pfree(additional_ctids);
	hash_destroy(doc_scores_hash);
	dshash_detach(string_table);

	return scored_count + additional_count;
}
