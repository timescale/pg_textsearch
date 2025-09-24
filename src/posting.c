/*-------------------------------------------------------------------------
 *
 * posting.c
 *	  Simplified posting list management for new architecture
 *
 * IDENTIFICATION
 *	  src/posting.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <lib/dshash.h>
#include <math.h>
#include <storage/itemptr.h>
#include <utils/dsa.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>

#include "constants.h"
#include "memtable.h"
#include "metapage.h"
#include "posting.h"
#include "state.h"
#include "stringtable.h"

/* Configuration parameters */
int tp_posting_list_growth_factor = TP_POSTING_LIST_GROWTH_FACTOR;

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

/* Helper function to get memtable from local index state */
static TpMemtable *
get_memtable(TpLocalIndexState *local_state)
{
	if (!local_state || !local_state->shared || !local_state->dsa)
		return NULL;

	if (!DsaPointerIsValid(local_state->shared->memtable_dp))
		return NULL;

	return (TpMemtable *)dsa_get_address(
			local_state->dsa, local_state->shared->memtable_dp);
}

/*
 * Add terms from a document to the posting lists
 * Simplified for new architecture - no locks needed
 */
void
tp_add_document_terms(
		TpLocalIndexState *local_state,
		ItemPointer		   ctid,
		char			 **terms,
		int32			  *frequencies,
		int				   term_count,
		int32			   doc_length)
{
	int i;

	for (i = 0; i < term_count; i++)
	{
		TpPostingList *posting_list;
		int32		   frequency = frequencies[i];

		/* Get or create posting list for this term */
		posting_list = tp_get_or_create_posting_list(local_state, terms[i]);

		/* Add document entry to posting list */
		tp_add_document_to_posting_list(
				local_state, posting_list, ctid, frequency);
	}

	/* Update corpus statistics (no locks needed with new architecture) */
	local_state->shared->total_docs++;
	local_state->shared->total_len += doc_length;
}

/*
 * Get posting list for a specific term
 * Returns NULL if term not found
 */
TpPostingList *
tp_get_posting_list(TpLocalIndexState *local_state, const char *term)
{
	TpMemtable		  *memtable;
	TpStringHashTable *string_table;
	TpStringHashEntry *string_entry;
	TpPostingList	  *posting_list;
	size_t			   term_len;

	Assert(local_state != NULL);

	if (!term)
	{
		elog(ERROR, "tp_get_posting_list: term is NULL");
		return NULL;
	}

	/* Get memtable from local state */
	memtable = get_memtable(local_state);
	if (!memtable)
	{
		elog(WARNING, "Cannot get memtable");
		return NULL;
	}

	/* Get the string hash table from the memtable */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
		return NULL;

	string_table = tp_hash_table_attach_dsa(
			local_state->dsa, memtable->string_hash_handle);
	if (!string_table)
	{
		elog(WARNING, "Failed to attach to string hash table");
		return NULL;
	}
	term_len = strlen(term);

	/* Look up the term in the string table (no locks needed) */
	string_entry =
			tp_hash_lookup_dsa(local_state->dsa, string_table, term, term_len);

	if (string_entry && DsaPointerIsValid(string_entry->key.flag_field))
	{
		posting_list = dsa_get_address(
				local_state->dsa, string_entry->key.flag_field);
		tp_hash_table_detach_dsa(string_table);
		return posting_list;
	}
	else
	{
		tp_hash_table_detach_dsa(string_table);
		return NULL;
	}
}

/*
 * Get or create a posting list for a term
 */
TpPostingList *
tp_get_or_create_posting_list(TpLocalIndexState *local_state, const char *term)
{
	TpMemtable		  *memtable;
	TpStringHashTable *string_table;
	TpStringHashEntry *string_entry;
	TpPostingList	  *posting_list;
	dsa_pointer		   posting_list_dp;
	size_t			   term_len;

	Assert(local_state != NULL);
	Assert(term != NULL);

	term_len = strlen(term);

	/* Get memtable from local state */
	memtable = get_memtable(local_state);
	if (!memtable)
	{
		elog(ERROR, "Cannot get memtable");
		return NULL;
	}

	/* Initialize string hash table if needed */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		/* Create new dshash table */
		string_table = tp_hash_table_create_dsa(local_state->dsa);
		if (!string_table)
		{
			elog(ERROR, "Failed to create string hash table");
			return NULL;
		}

		/* Store the handle for other processes */
		memtable->string_hash_handle = tp_hash_table_get_handle(string_table);
	}
	else
	{
		/* Attach to existing table */
		string_table = tp_hash_table_attach_dsa(
				local_state->dsa, memtable->string_hash_handle);
		if (!string_table)
		{
			elog(ERROR, "Failed to attach to string hash table");
			return NULL;
		}
	}

	/* Look up or insert the term in the string table */
	string_entry =
			tp_hash_lookup_dsa(local_state->dsa, string_table, term, term_len);

	if (!string_entry)
	{
		/* Insert the term */
		string_entry = tp_hash_insert_dsa(
				local_state->dsa, string_table, term, term_len);
		if (!string_entry)
		{
			elog(ERROR, "Failed to insert term '%s' into string table", term);
			return NULL;
		}
	}

	/* Check if posting list already exists for this term */
	if (DsaPointerIsValid(string_entry->key.flag_field))
	{
		posting_list = dsa_get_address(
				local_state->dsa, string_entry->key.flag_field);
	}
	else
	{
		/* Create new posting list */
		posting_list_dp =
				dsa_allocate(local_state->dsa, sizeof(TpPostingList));
		posting_list = dsa_get_address(local_state->dsa, posting_list_dp);

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

	return posting_list;
}

/*
 * Add a document entry to a posting list - simplified
 */
void
tp_add_document_to_posting_list(
		TpLocalIndexState *local_state,
		TpPostingList	  *posting_list,
		ItemPointer		   ctid,
		int32			   frequency)
{
	TpPostingEntry *entries;
	TpPostingEntry *new_entry;
	dsa_pointer		new_entries_dp;

	Assert(local_state != NULL);
	Assert(posting_list != NULL);
	Assert(ItemPointerIsValid(ctid));

	/* Expand array if needed */
	if (posting_list->doc_count >= posting_list->capacity)
	{
		int32 new_capacity = posting_list->capacity == 0
								   ? TP_INITIAL_POSTING_LIST_CAPACITY
								   : posting_list->capacity *
											 tp_posting_list_growth_factor;

		/* Allocate new array */
		new_entries_dp = dsa_allocate(
				local_state->dsa, new_capacity * sizeof(TpPostingEntry));

		/* Copy existing entries if any */
		if (posting_list->doc_count > 0 &&
			DsaPointerIsValid(posting_list->entries_dp))
		{
			TpPostingEntry *old_entries =
					tp_get_posting_entries(local_state->dsa, posting_list);
			TpPostingEntry *new_entries =
					dsa_get_address(local_state->dsa, new_entries_dp);
			memcpy(new_entries,
				   old_entries,
				   posting_list->doc_count * sizeof(TpPostingEntry));

			/* Free old array */
			dsa_free(local_state->dsa, posting_list->entries_dp);
		}

		posting_list->entries_dp = new_entries_dp;
		posting_list->capacity	 = new_capacity;
	}

	/* Add new document entry */
	entries			= tp_get_posting_entries(local_state->dsa, posting_list);
	new_entry		= &entries[posting_list->doc_count];
	new_entry->ctid = *ctid;
	new_entry->frequency = frequency;

	posting_list->doc_count++;
	posting_list->doc_freq = posting_list->doc_count;
}

/*
 * Centralized IDF calculation - simplified
 */
float4
tp_calculate_idf(int32 doc_freq, int32 total_docs)
{
	double idf_numerator   = (double)(total_docs - doc_freq + 0.5);
	double idf_denominator = (double)(doc_freq + 0.5);
	double idf_ratio	   = idf_numerator / idf_denominator;
	double raw_idf		   = log(idf_ratio);

	/* Return raw IDF, ensuring it's at least 0 */
	return (float4)fmax(raw_idf, 0.0);
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
	TpMemtable		  *memtable;
	TpStringHashTable *string_table;
	float4			  *scores;
	int32			   total_docs;
	int64			   total_len;
	float4			   avg_doc_len;
	int				   result_count = 0;
	int				   i, j;

	Assert(local_state != NULL);
	Assert(query_terms != NULL);
	Assert(query_frequencies != NULL);
	Assert(result_ctids != NULL);
	Assert(result_scores != NULL);

	if (query_term_count <= 0 || max_results <= 0)
		return 0;

	/* Get memtable and corpus statistics */
	memtable = get_memtable(local_state);
	if (!memtable)
	{
		elog(WARNING, "Cannot get memtable for scoring");
		return 0;
	}

	total_docs	= local_state->shared->total_docs;
	total_len	= local_state->shared->total_len;
	avg_doc_len = total_docs > 0 ? (float4)(total_len / (double)total_docs)
								 : 0.0f;

	if (total_docs <= 0)
		return 0;

	/* Get string hash table */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
		return 0;

	string_table = tp_hash_table_attach_dsa(
			local_state->dsa, memtable->string_hash_handle);
	if (!string_table)
		return 0;

	/* Allocate scores array */
	scores		   = (float4 *)palloc0(max_results * sizeof(float4));
	*result_scores = scores;

	/* Simple implementation: score the first max_results documents in posting
	 * lists */
	for (i = 0; i < query_term_count && result_count < max_results; i++)
	{
		TpPostingList  *posting_list;
		TpPostingEntry *entries;
		float4			idf;

		posting_list = tp_get_posting_list(local_state, query_terms[i]);
		if (!posting_list || posting_list->doc_count <= 0)
			continue;

		/* Calculate IDF for this term */
		idf = tp_calculate_idf(posting_list->doc_count, total_docs);

		/* Get posting entries */
		entries = tp_get_posting_entries(local_state->dsa, posting_list);
		if (!entries)
			continue;

		/* Score documents in this posting list */
		for (j = 0; j < posting_list->doc_count && result_count < max_results;
			 j++)
		{
			float4 tf		  = (float4)entries[j].frequency;
			float4 doc_length = 1.0f; /* TODO: Get actual document length */
			float4 normalized_tf;
			float4 bm25_score;

			/* BM25 formula: IDF * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b *
			 * (doc_length / avg_doc_len))) */
			normalized_tf = (tf * (k1 + 1.0f)) /
							(tf +
							 k1 * (1.0f - b + b * (doc_length / avg_doc_len)));
			bm25_score = idf * normalized_tf;

			/* Store result */
			result_ctids[result_count] = entries[j].ctid;
			scores[result_count] =
					-bm25_score; /* Negative for PostgreSQL ASC ordering */
			result_count++;
		}
	}

	tp_hash_table_detach_dsa(string_table);
	return result_count;
}

/*
 * Get document length - simplified stub
 */
int32
tp_get_document_length(TpLocalIndexState *local_state, ItemPointer ctid)
{
	(void)local_state;
	(void)ctid;

	/* Stub implementation - return default length */
	return 1;
}

/*
 * Shared memory cleanup - simplified stub
 */
void
tp_cleanup_index_shared_memory(Oid index_oid)
{
	(void)index_oid;
	/* Cleanup is handled by tp_destroy_shared_index_state */
}
