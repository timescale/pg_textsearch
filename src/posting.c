/*
 * posting.c - Tapir In-Memory Posting Lists Implementation
 *
 * This module implements in-memory posting lists for using standard
 * PostgreSQL hash tables.  Eventually, they will be flushed to disk
 * when full.
 */

#include <postgres.h>

#include <access/heapam.h>
#include <access/relscan.h>
#include <math.h>
#include <miscadmin.h>
#include <storage/lwlock.h>
#include <utils/dsa.h>
#include <utils/hsearch.h>
#include <utils/snapmgr.h>

#include "constants.h"
#include "memtable.h"
#include "metapage.h"
#include "posting.h"
#include "stringtable.h"

/* Configuration parameters */
int tp_posting_list_growth_factor = TP_POSTING_LIST_GROWTH_FACTOR;

/* Forward declarations */
TpPostingList *
tp_get_or_create_posting_list(TpIndexState *index_state, const char *term);

/* Helper function to get entries array from posting list */
static TpPostingEntry *
tp_get_posting_entries(dsa_area *area, TpPostingList *posting_list)
{
	if (!posting_list || !DsaPointerIsValid(posting_list->entries_dp))
		return NULL;
	if (!area)
		return NULL;
	return dsa_get_address(area, posting_list->entries_dp);
}

/*
 * Add terms from a document to the posting lists
 * This is the core operation for building the inverted index
 * Uses DSA-based string table functions for storage
 */
void
tp_add_document_terms(
		TpIndexState *index_state,
		ItemPointer	  ctid,
		char		**terms,
		int32		 *frequencies,
		int			  term_count,
		int32		  doc_length)
{
	int i;

	Assert(index_state != NULL);
	Assert(ctid != NULL);
	Assert(terms != NULL);
	Assert(frequencies != NULL);

	/* Process each term */
	for (i = 0; i < term_count; i++)
	{
		char		  *term		 = terms[i];
		int32		   frequency = frequencies[i];
		TpPostingList *posting_list;

		if (!term || frequency <= 0)
			continue;

		/* Get or create posting list for this term through string interning */
		posting_list = tp_get_or_create_posting_list(index_state, term);
		if (!posting_list)
		{
			elog(WARNING,
				 "Failed to get/create posting list for term '%s'",
				 term);
			continue;
		}

		/* Add document entry to posting list */
		tp_add_document_to_posting_list(
				index_state, posting_list, ctid, frequency);
	}

	/* Store document length in hash table (once per document, not per term) */
	if (index_state->doc_lengths_hash)
	{
		TpDocLengthEntry  doc_entry;
		TpDocLengthEntry *existing_entry;
		bool			  found;

		/* Set up the entry */
		doc_entry.ctid		 = *ctid;
		doc_entry.doc_length = doc_length;

		/* Insert into hash table */
		LWLockAcquire(&index_state->doc_lengths_lock, LW_EXCLUSIVE);
		existing_entry = (TpDocLengthEntry *)dshash_find_or_insert(
				index_state->doc_lengths_hash, &doc_entry.ctid, &found);

		if (!found)
		{
			/* New document - store the length */
			existing_entry->doc_length = doc_length;
		}

		LWLockRelease(&index_state->doc_lengths_lock);
	}

	/* Update corpus statistics */
	LWLockAcquire(&index_state->corpus_stats_lock, LW_EXCLUSIVE);
	index_state->stats.total_docs++;
	index_state->stats.total_terms += term_count;

	/* Update document length statistics */
	index_state->stats.total_len += doc_length;

	if (index_state->stats.total_docs == 1)
	{
		index_state->stats.min_doc_length = doc_length;
		index_state->stats.max_doc_length = doc_length;
	}
	else
	{
		if (doc_length < index_state->stats.min_doc_length)
			index_state->stats.min_doc_length = doc_length;
		if (doc_length > index_state->stats.max_doc_length)
			index_state->stats.max_doc_length = doc_length;
	}

	LWLockRelease(&index_state->corpus_stats_lock);
}

/*
 * Get posting list for a specific term
 * Returns NULL if term not found
 * Uses DSA-based string interning table
 */
TpPostingList *
tp_get_posting_list(TpIndexState *index_state, const char *term)
{
	TpStringHashTable *string_table;
	TpStringHashEntry *string_entry;
	TpPostingList	  *posting_list;
	dsa_area		  *area;
	size_t			   term_len;

	Assert(index_state != NULL);

	if (!term)
	{
		elog(ERROR, "tp_get_posting_list: term is NULL");
		return NULL;
	}

	/* Get DSA area for this index using cached approach */
	area = tp_get_dsa_area_for_index(index_state, InvalidOid);
	if (!area)
	{
		elog(WARNING, "Cannot get DSA area for index");
		return NULL;
	}

	/* Get the string hash table from the index state */
	if (index_state->string_hash_handle == DSHASH_HANDLE_INVALID)
		return NULL;

	string_table =
			tp_hash_table_attach_dsa(area, index_state->string_hash_handle);
	if (!string_table)
	{
		elog(WARNING, "Failed to attach to string hash table");
		return NULL;
	}
	term_len = strlen(term);

	/* Look up the term in the string table */
	LWLockAcquire(&index_state->string_interning_lock, LW_SHARED);
	string_entry = tp_hash_lookup_dsa(area, string_table, term, term_len);

	if (string_entry && DsaPointerIsValid(string_entry->key.flag_field))
	{
		posting_list = dsa_get_address(area, string_entry->key.flag_field);
		tp_hash_table_detach_dsa(string_table);
		LWLockRelease(&index_state->string_interning_lock);
		return posting_list;
	}
	else
	{
		tp_hash_table_detach_dsa(string_table);
		LWLockRelease(&index_state->string_interning_lock);
		return NULL;
	}
}

/*
 * Get or create a posting list for a term
 * Uses DSA-based string interning table
 */
TpPostingList *
tp_get_or_create_posting_list(TpIndexState *index_state, const char *term)
{
	TpStringHashTable *string_table;
	TpStringHashEntry *string_entry;
	TpPostingList	  *posting_list;
	dsa_area		  *area;
	dsa_pointer		   posting_list_dp;
	size_t			   term_len;

	Assert(index_state != NULL);
	Assert(term != NULL);

	term_len = strlen(term);

	/* Get DSA area for this index using cached approach */
	area = tp_get_dsa_area_for_index(index_state, InvalidOid);
	if (!area)
	{
		elog(ERROR,
			 "Cannot get DSA area for index %u",
			 index_state->index_oid);
		return NULL;
	}

	/* Initialize string hash table if needed */
	LWLockAcquire(&index_state->string_interning_lock, LW_EXCLUSIVE);

	if (index_state->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		/* Create new dshash table */
		string_table = tp_hash_table_create_dsa(area);
		if (!string_table)
		{
			LWLockRelease(&index_state->string_interning_lock);
			elog(ERROR, "Failed to create string hash table");
			return NULL;
		}

		/* Store the handle for other processes */
		index_state->string_hash_handle = tp_hash_table_get_handle(
				string_table);
	}
	else
	{
		/* Attach to existing table */
		string_table = tp_hash_table_attach_dsa(
				area, index_state->string_hash_handle);
		if (!string_table)
		{
			LWLockRelease(&index_state->string_interning_lock);
			elog(ERROR, "Failed to attach to string hash table");
			return NULL;
		}
	}

	/* Look up or insert the term in the string table */
	string_entry = tp_hash_lookup_dsa(area, string_table, term, term_len);

	if (!string_entry)
	{
		/* Insert the term */
		string_entry = tp_hash_insert_dsa(area, string_table, term, term_len);
		if (!string_entry)
		{
			LWLockRelease(&index_state->string_interning_lock);
			elog(ERROR, "Failed to insert term '%s' into string table", term);
			return NULL;
		}
	}

	/* Check if posting list already exists for this term */
	if (DsaPointerIsValid(string_entry->key.flag_field))
	{
		posting_list = dsa_get_address(area, string_entry->key.flag_field);
	}
	else
	{
		/* Create new posting list */
		posting_list_dp = dsa_allocate(area, sizeof(TpPostingList));
		posting_list	= dsa_get_address(area, posting_list_dp);

		/* Initialize posting list */
		memset(posting_list, 0, sizeof(TpPostingList));
		posting_list->doc_count	   = 0;
		posting_list->capacity	   = 0;
		posting_list->is_finalized = false;
		posting_list->doc_freq	   = 0;
		posting_list->entries_dp   = InvalidDsaPointer;

		/* Associate posting list with string entry */
		string_entry->key.flag_field   = posting_list_dp;
		string_entry->posting_list_len = 0;
	}

	/* Detach from string table */
	tp_hash_table_detach_dsa(string_table);

	LWLockRelease(&index_state->string_interning_lock);
	return posting_list;
}

/*
 * Add a document entry to a posting list
 * Updates the document frequency and stores the document information
 */
void
tp_add_document_to_posting_list(
		TpIndexState  *index_state,
		TpPostingList *posting_list,
		ItemPointer	   ctid,
		int32		   frequency)
{
	TpPostingEntry *entries;
	TpPostingEntry *new_entry;
	dsa_area	   *area;
	dsa_pointer		new_entries_dp;

	if (!posting_list || !index_state)
		return;

	/* Get DSA area from index state */
	area = tp_get_dsa_area_for_index(index_state, InvalidOid);
	if (!area)
	{
		elog(WARNING,
			 "Cannot get DSA area for adding document to posting list");
		return;
	}

	/* Ensure we have capacity for the new entry */
	if (posting_list->doc_count >= posting_list->capacity)
	{
		/* Need to expand the entries array */
		int32 new_capacity = posting_list->capacity == 0
								   ? 8
								   : posting_list->capacity * 2;

		/* Allocate new array with expanded capacity */
		new_entries_dp =
				dsa_allocate(area, sizeof(TpPostingEntry) * new_capacity);
		if (!DsaPointerIsValid(new_entries_dp))
		{
			elog(WARNING,
				 "Failed to allocate DSA memory for posting list expansion");
			return;
		}

		/* If we had existing entries, copy them to the new array */
		if (DsaPointerIsValid(posting_list->entries_dp) &&
			posting_list->doc_count > 0)
		{
			TpPostingEntry *old_entries =
					dsa_get_address(area, posting_list->entries_dp);
			TpPostingEntry *new_entries =
					dsa_get_address(area, new_entries_dp);

			memcpy(new_entries,
				   old_entries,
				   sizeof(TpPostingEntry) * posting_list->doc_count);

			/* Free old array */
			dsa_free(area, posting_list->entries_dp);
		}

		/* Update posting list to use new array */
		posting_list->entries_dp = new_entries_dp;
		posting_list->capacity	 = new_capacity;
	}

	/* Initialize entries array if this is the first document */
	if (!DsaPointerIsValid(posting_list->entries_dp))
	{
		posting_list->entries_dp =
				dsa_allocate(area, sizeof(TpPostingEntry) * 8);
		posting_list->capacity = 8;
		if (!DsaPointerIsValid(posting_list->entries_dp))
		{
			elog(WARNING,
				 "Failed to allocate initial DSA memory for posting list");
			return;
		}
	}

	/* Get the entries array and add the new document */
	entries	  = dsa_get_address(area, posting_list->entries_dp);
	new_entry = &entries[posting_list->doc_count];

	/* Store document information */
	new_entry->ctid	  = *ctid;
	new_entry->doc_id = posting_list->doc_count; /* Use doc_count as simple
												  * doc_id */
	new_entry->frequency = frequency;
	/* Note: doc_length now stored in separate hash table */

	/* Update posting list counters */
	posting_list->doc_count++;
	posting_list->doc_freq = posting_list->doc_count;
}

/*
 * Centralized IDF calculation with epsilon flooring
 *
 * Calculates IDF using the standard BM25 formula: log((N - df + 0.5) / (df +
 * 0.5)) If the result is negative or zero, applies epsilon flooring like
 * Python BM25: epsilon = 0.25 * average_idf
 */
float4
tp_calculate_idf(int32 doc_freq, int32 total_docs, float4 average_idf)
{
	double idf_numerator   = (double)(total_docs - doc_freq + 0.5);
	double idf_denominator = (double)(doc_freq + 0.5);
	double idf_ratio	   = idf_numerator / idf_denominator;
	double raw_idf		   = log(idf_ratio);

	if (raw_idf <= 0.0)
	{
		/*
		 * Apply epsilon flooring like Python BM25: epsilon = 0.25 *
		 * average_idf
		 */
		float4 epsilon = 0.25f * average_idf;

		return epsilon;
	}
	else
	{
		return (float4)raw_idf;
	}
}

/*
 * Calculate average IDF across all terms in the index
 * This is used for epsilon flooring in BM25 calculations to match Python BM25
 * behavior
 */
void
tp_calculate_average_idf(TpIndexState *index_state)
{
	TpStringHashTable *string_table;
	dsa_area		  *area;
	uint32			   term_count = 0;
	double			   idf_sum	  = 0.0;
	int32			   total_docs = index_state->stats.total_docs;

	/* Skip calculation if no documents */
	if (total_docs <= 0)
	{
		index_state->stats.average_idf = 0.0f;
		return;
	}

	area = tp_get_dsa_area_for_index(index_state, InvalidOid);
	if (!area)
	{
		elog(WARNING, "Cannot access DSA area for average IDF calculation");
		index_state->stats.average_idf = 0.0f;
		return;
	}

	/* Get the string hash table */
	if (index_state->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		index_state->stats.average_idf = 0.0f;
		return;
	}

	string_table =
			tp_hash_table_attach_dsa(area, index_state->string_hash_handle);
	if (!string_table)
	{
		elog(WARNING,
			 "Failed to attach to string hash table for IDF calculation");
		index_state->stats.average_idf = 0.0f;
		return;
	}

	/* Iterate through all entries using dshash sequential scan */
	{
		dshash_seq_status  status;
		TpStringHashEntry *entry;

		dshash_seq_init(
				&status, string_table->dshash, false); /* shared lock */

		while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) != NULL)
		{
			/* Calculate IDF for this term if it has a posting list */
			if (DsaPointerIsValid(entry->key.flag_field))
			{
				TpPostingList *posting_list =
						dsa_get_address(area, entry->key.flag_field);
				int32 doc_freq = posting_list->doc_count;

				if (doc_freq > 0)
				{
					double idf_numerator   = (double)(total_docs - doc_freq +
													  0.5);
					double idf_denominator = (double)(doc_freq + 0.5);
					double idf_ratio	   = idf_numerator / idf_denominator;
					double idf			   = log(idf_ratio);

					idf_sum += idf;
					term_count++;
				}
			}
		}

		dshash_seq_term(&status);
	}
	tp_hash_table_detach_dsa(string_table);

	/* Calculate average IDF */
	if (term_count > 0)
	{
		index_state->stats.average_idf = (float4)(idf_sum /
												  (double)term_count);
	}
	else
	{
		index_state->stats.average_idf = 0.0f;
	}
}

/*
 * Finalize index building by calculating corpus statistics
 * This converts from the building phase to query-ready phase
 * Uses DSA-based system for shared memory storage
 */
void
tp_finalize_index_build(TpIndexState *index_state)
{
	Assert(index_state != NULL);

	/* Calculate average IDF for epsilon flooring */
	tp_calculate_average_idf(index_state);
}

/*
 * Single-pass BM25 scoring for multiple documents
 * This is much more efficient than calling bm25_score_document for each
 * document Complexity: O(n × q) where n = total posting entries, q = query
 * terms
 */
typedef struct DocumentScoreEntry
{
	ItemPointerData ctid;		/* Document CTID (hash key) */
	float4			score;		/* Accumulated BM25 score */
	float4			doc_length; /* Document length (for BM25 calculation) */
} DocumentScoreEntry;

int
tp_score_documents(
		TpIndexState *index_state,
		Relation	  index_relation,
		char		**query_terms,
		int32		 *query_frequencies,
		int			  query_term_count,
		float4		  k1,
		float4		  b,
		int			  max_results,
		ItemPointer	  result_ctids,
		float4		**result_scores)
{
	HASHCTL hash_ctl;

	HTAB				*doc_scores_hash;
	DocumentScoreEntry	*doc_entry;
	float4				 avg_doc_len;
	int32				 total_docs;
	bool				 found;
	int					 result_count = 0;
	DocumentScoreEntry **sorted_docs;
	int					 i, j;
	int					 hash_size;

	/* Validate inputs */
	if (!index_state)
		elog(ERROR, "tp_score_documents: NULL index_state");
	if (!query_terms)
		elog(ERROR, "tp_score_documents: NULL query_terms");
	if (!query_frequencies)
		elog(ERROR, "tp_score_documents: NULL query_frequencies");
	if (!result_ctids)
		elog(ERROR, "tp_score_documents: NULL result_ctids");
	if (!result_scores)
		elog(ERROR, "tp_score_documents: NULL result_scores");

	total_docs	= index_state->stats.total_docs;
	avg_doc_len = total_docs > 0 ? (float4)(index_state->stats.total_len /
											(double)total_docs)
								 : 0.0f;

	/* Create hash table for accumulating document scores */

	/* Ensure reasonable hash table size with upper bounds */
	hash_size = max_results * 2;
	if (total_docs > 0 && hash_size > total_docs * 2)
		hash_size = total_docs * 2;
	if (hash_size < 128)
		hash_size = 128; /* Minimum reasonable size */
	if (hash_size > 10000)
		hash_size = 10000; /* Maximum reasonable size to prevent memory
							* issues */

	/* Initialize hash control structure */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(DocumentScoreEntry);

	/* Create hash table */
	doc_scores_hash = hash_create(
			"DocScores", hash_size, &hash_ctl, HASH_ELEM | HASH_BLOBS);

	if (!doc_scores_hash)
		elog(ERROR, "Failed to create document scores hash table");

	/* Single pass through all posting lists */
	for (int term_idx = 0; term_idx < query_term_count; term_idx++)
	{
		const char	   *term = query_terms[term_idx];
		TpPostingList  *posting_list;
		TpPostingEntry *entries;
		float4			idf;

		posting_list = tp_get_posting_list(index_state, term);

		if (!posting_list || posting_list->doc_count == 0)
		{
			continue;
		}

		/* Calculate IDF using centralized function with epsilon flooring */
		idf = tp_calculate_idf(
				posting_list->doc_count,
				total_docs,
				index_state->stats.average_idf);

		/* Process each document in this term's posting list */
		/* Get DSA area and retrieve entries */
		{
			dsa_area *area =
					tp_get_dsa_area_for_index(index_state, InvalidOid);

			if (!area)
				continue; /* Skip if can't get DSA area */
			entries = tp_get_posting_entries(area, posting_list);
			if (!entries)
				continue; /* Skip if no entries stored yet */
		}
		for (int doc_idx = 0; doc_idx < posting_list->doc_count; doc_idx++)
		{
			TpPostingEntry *entry = &entries[doc_idx];
			float4			tf	  = entry->frequency;
			float4			doc_len;
			float4			term_score;
			double			numerator_d, denominator_d;

			/* Look up document length from hash table */
			doc_len = (float4)
					tp_get_document_length(index_state, &entry->ctid);

			/* Validate TID */
			if (!ItemPointerIsValid(&entry->ctid))
				continue;

			/* Calculate BM25 term score contribution */
			numerator_d = (double)tf * ((double)k1 + 1.0);

			/*
			 * Avoid division by zero - if avg_doc_len is 0, use doc_len
			 * directly
			 */
			if (avg_doc_len > 0.0f)
			{
				denominator_d = (double)tf +
								(double)k1 *
										(1.0 - (double)b +
										 (double)b * ((double)doc_len /
													  (double)avg_doc_len));
			}
			else
			{
				/*
				 * When avg_doc_len is 0 (no corpus stats), fall back to
				 * standard TF formula
				 */
				denominator_d = (double)tf + (double)k1;
			}

			term_score = (float4)((double)idf * (numerator_d / denominator_d) *
								  (double)query_frequencies[term_idx]);

			/* Debug NaN detection */
			if (isnan(term_score))
			{
				elog(LOG,
					 "NaN detected in term_score calculation: idf=%f, "
					 "numerator_d=%f, "
					 "denominator_d=%f, query_freq=%d, tf=%f, doc_len=%f, "
					 "avg_doc_len=%f, k1=%f, "
					 "b=%f",
					 idf,
					 numerator_d,
					 denominator_d,
					 query_frequencies[term_idx],
					 tf,
					 doc_len,
					 avg_doc_len,
					 k1,
					 b);
			}

			/* Find or create document entry in hash table */
			doc_entry = (DocumentScoreEntry *)hash_search(
					doc_scores_hash, &entry->ctid, HASH_ENTER, &found);

			if (!found)
			{
				/* New document - initialize */
				doc_entry->ctid		  = entry->ctid;
				doc_entry->score	  = term_score; /* Positive BM25 score */
				doc_entry->doc_length = doc_len;
			}
			else
			{
				/* Existing document - accumulate score */
				doc_entry->score += term_score; /* Positive BM25 score */
			}
		}
	}

	/* Extract and sort documents by score */
	/* First, count how many unique documents we have */
	result_count = hash_get_num_entries(doc_scores_hash);

	if (result_count > max_results)
		result_count = max_results;

	/* Allocate array for sorting (even if result_count is 0) */
	sorted_docs = result_count > 0
						? palloc(result_count * sizeof(DocumentScoreEntry *))
						: NULL;

	/* Extract documents from hash table using hash_seq_search */
	i = 0;
	if (result_count > 0)
	{
		HASH_SEQ_STATUS		seq_status;
		DocumentScoreEntry *current_entry;

		hash_seq_init(&seq_status, doc_scores_hash);

		while ((current_entry = (DocumentScoreEntry *)hash_seq_search(
						&seq_status)) != NULL &&
			   i < result_count)
		{
			sorted_docs[i++] = current_entry;
		}

		/*
		 * If we hit the result_count limit while current_entry was still
		 * non-NULL, we need to terminate the scan manually. If
		 * hash_seq_search returned NULL, the scan completed naturally and
		 * cleanup was done automatically.
		 */
		if (current_entry != NULL && i >= result_count)
			hash_seq_term(&seq_status);
	}

	result_count = i; /* Actual count extracted */

	/* Sort documents by score (descending - higher scores first) */
	for (i = 1; i < result_count; i++)
	{
		DocumentScoreEntry *key = sorted_docs[i];

		j = i - 1;

		while (j >= 0 && sorted_docs[j]->score < key->score)
		{
			sorted_docs[j + 1] = sorted_docs[j];
			j--;
		}
		sorted_docs[j + 1] = key;
	}

	/* Fill remaining slots with zero-scored documents if needed */
	if (result_count < max_results && index_relation != NULL)
	{
		TpIndexMetaPage metap;
		BlockNumber		current_page;
		int				additional_count = 0;
		ItemPointer		additional_ctids = NULL;
		HTAB		   *seen_docs_hash	 = NULL;
		HASHCTL			seen_docs_ctl;

		/* Get metapage to find first docid page */
		metap = tp_get_metapage(index_relation);

		if (metap != NULL && metap->first_docid_page != InvalidBlockNumber)
		{
			/* Create hash table to track documents we've already included */
			memset(&seen_docs_ctl, 0, sizeof(seen_docs_ctl));
			seen_docs_ctl.keysize	= sizeof(ItemPointerData);
			seen_docs_ctl.entrysize = sizeof(ItemPointerData);
			seen_docs_ctl.hcxt		= CurrentMemoryContext;

			seen_docs_hash = hash_create(
					"seen_documents",
					result_count + max_results,
					&seen_docs_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

			/* Add already scored documents to seen hash */
			for (i = 0; i < result_count; i++)
			{
				hash_search(
						seen_docs_hash,
						&sorted_docs[i]->ctid,
						HASH_ENTER,
						NULL);
			}

			/* Allocate space for additional documents */
			additional_ctids = palloc(
					(max_results - result_count) * sizeof(ItemPointerData));

			/* Iterate through docid pages to find additional documents */
			current_page = metap->first_docid_page;

			while (current_page != InvalidBlockNumber &&
				   additional_count < (max_results - result_count))
			{
				Buffer			   docid_buf;
				Page			   docid_page;
				TpDocidPageHeader *docid_header;
				ItemPointer		   docids;

				docid_buf = ReadBuffer(index_relation, current_page);
				LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
				docid_page	 = BufferGetPage(docid_buf);
				docid_header = (TpDocidPageHeader *)PageGetContents(
						docid_page);

				/* Validate page magic */
				if (docid_header->magic != TP_DOCID_PAGE_MAGIC)
				{
					UnlockReleaseBuffer(docid_buf);
					break;
				}

				/* Process each docid on this page */
				docids = (ItemPointer)((char *)docid_header +
									   sizeof(TpDocidPageHeader));

				for (i = 0; i < (int)docid_header->num_docids &&
							additional_count < (max_results - result_count);
					 i++)
				{
					ItemPointer ctid = &docids[i];
					bool		doc_found;

					/* Check if we've already included this document */
					hash_search(seen_docs_hash, ctid, HASH_FIND, &doc_found);

					if (!doc_found)
					{
						/* Add this document with zero score */
						additional_ctids[additional_count] = *ctid;
						additional_count++;

						/* Mark as seen to avoid duplicates */
						hash_search(seen_docs_hash, ctid, HASH_ENTER, NULL);
					}
				}

				/* Move to next page */
				current_page = docid_header->next_page;
				UnlockReleaseBuffer(docid_buf);
			}

			/* Clean up */
			if (seen_docs_hash)
				hash_destroy(seen_docs_hash);
		}

		/* Combine scored and zero-scored results */
		if (additional_count > 0)
		{
			int total_results = result_count + additional_count;

			*result_scores = palloc(total_results * sizeof(float4));

			/* Copy scored results */
			for (i = 0; i < result_count; i++)
			{
				result_ctids[i]		= sorted_docs[i]->ctid;
				(*result_scores)[i] = sorted_docs[i]->score;
			}

			/* Add zero-scored results */
			for (i = 0; i < additional_count; i++)
			{
				result_ctids[result_count + i]	   = additional_ctids[i];
				(*result_scores)[result_count + i] = 0.0;
			}

			result_count = total_results;
			pfree(additional_ctids);
		}
		else
		{
			/* No additional documents, use original allocation */
			if (result_count > 0)
			{
				*result_scores = palloc(result_count * sizeof(float4));
				for (i = 0; i < result_count; i++)
				{
					result_ctids[i]		= sorted_docs[i]->ctid;
					(*result_scores)[i] = sorted_docs[i]->score;
				}
			}
			else
			{
				*result_scores = NULL;
			}
		}

		/* Clean up metapage */
		if (metap)
			pfree(metap);
	}
	else
	{
		/* Copy sorted results to output arrays */
		if (result_count > 0)
		{
			*result_scores = palloc(result_count * sizeof(float4));
			for (i = 0; i < result_count; i++)
			{
				result_ctids[i]		= sorted_docs[i]->ctid;
				(*result_scores)[i] = sorted_docs[i]->score;
			}
		}
		else
		{
			*result_scores = NULL;
		}
	}

	/* Cleanup */
	if (sorted_docs)
		pfree(sorted_docs);
	hash_destroy(doc_scores_hash);

	return result_count;
}

/*
 * Get corpus statistics
 */
TpCorpusStatistics *
tp_get_corpus_statistics(TpIndexState *index_state)
{
	Assert(index_state != NULL);
	return &index_state->stats;
}

/*
 * Look up document length from the document length hash table
 * Returns the document length, or 0 if not found
 */
int32
tp_get_document_length(TpIndexState *index_state, ItemPointer ctid)
{
	TpDocLengthEntry *entry;
	int32			  doc_length = 0;

	if (!index_state->doc_lengths_hash)
	{
		return 0;
	}

	LWLockAcquire(&index_state->doc_lengths_lock, LW_SHARED);
	entry = (TpDocLengthEntry *)
			dshash_find(index_state->doc_lengths_hash, ctid, false);
	if (entry)
	{
		doc_length = entry->doc_length;
	}
	LWLockRelease(&index_state->doc_lengths_lock);

	return doc_length;
}
