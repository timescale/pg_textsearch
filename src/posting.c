/*
 * posting.c - Tapir In-Memory Posting Lists Implementation
 *
 * This module implements in-memory posting lists for using standard
 * PostgreSQL hash tables.  Eventually, they will be flushed to disk
 * when full.
 */

#include "postgres.h"

#include "constants.h"
#include "posting.h"
#include "memtable.h"
#include "stringtable.h"
#include "index.h"
#include "vector.h"
#include "miscadmin.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/elog.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include <math.h>

/* Global shared state - removed for DSA-based system */
/* Each index now manages its own DSA segment */

/* Configuration parameters */
int			tp_posting_list_growth_factor = TP_POSTING_LIST_GROWTH_FACTOR;	/* Double size when
													 * growing arrays */

/* Forward declarations */
TpPostingList *tp_get_or_create_posting_list(TpIndexState *index_state, const char *term);

/* Helper function to get entries array from posting list */
static TpPostingEntry *tp_get_posting_entries(dsa_area *area, TpPostingList *posting_list)
{
	if (!posting_list || !DsaPointerIsValid(posting_list->entries_dp))
		return NULL;
	if (!area) {
		elog(DEBUG3, "DSA area not provided to tp_get_posting_entries");
		return NULL;
	}
	return dsa_get_address(area, posting_list->entries_dp);
}


/*
 * Get or create index state for a specific Tapir index
 * This creates per-index shared memory structures
 */
TpIndexState *
tp_get_index_state(Oid index_oid, const char *index_name)
{
	TpIndexState *index_state;
	dsa_area *area = NULL;

	/* Use DSA-based index management with built-in caching */
	index_state = tp_get_or_create_index_dsa(index_oid, &area);

	if (!index_state) {
		elog(ERROR, "Could not attach to DSA for index %u", index_oid);
		return NULL;
	}

	if (!area) {
		elog(WARNING, "DSA area not returned for index %u - may cause query failures", index_oid);
	}

	/* Load configuration parameters from index metapage */
	{
		Relation index_rel;
		Buffer metabuf;
		Page metapage;
		TpIndexMetaPage meta;

		/* Open the index relation to read its metapage */
		index_rel = index_open(index_oid, AccessShareLock);

		/* Read the metapage */
		metabuf = ReadBuffer(index_rel, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_SHARE);
		metapage = BufferGetPage(metabuf);
		meta = (TpIndexMetaPage) PageGetContents(metapage);

		/* Copy configuration parameters from metapage to DSA structure */
		LWLockAcquire(&index_state->corpus_stats_lock, LW_EXCLUSIVE);

		/* CRITICAL FIX: Do NOT load corpus statistics from metapage!
		 * This was causing data consistency issues where live DSA statistics
		 * were being overwritten with stale metapage values.
		 *
		 * Corpus statistics (total_docs, total_len) should ONLY be managed
		 * in DSA shared memory and persist there. The metapage values are stale.
		 */

		/* Store k1 and b for BM25 scoring */
		index_state->stats.k1 = meta->k1;
		index_state->stats.b = meta->b;
		LWLockRelease(&index_state->corpus_stats_lock);

		elog(DEBUG1, "Loaded configuration from metapage for index %s: k1=%f, b=%f (corpus stats preserved in DSA)",
			 index_name, index_state->stats.k1, index_state->stats.b);

		UnlockReleaseBuffer(metabuf);

		index_close(index_rel, AccessShareLock);
	}

	/* DSA-based system doesn't track per-index memory counters in the old way */
	/* These fields no longer exist in TpIndexState */

	/* Debug: Check string_hash_handle state after DSA attachment */
	elog(DEBUG1, "After DSA attachment: string_hash_handle=%s, total_docs=%d",
		 (index_state->string_hash_handle != DSHASH_HANDLE_INVALID) ? "VALID" : "INVALID",
		 index_state->stats.total_docs);

	elog(DEBUG1, "Attached to Tapir index state for %s",
		 index_name);
	return index_state;
}

/* tp_rebuild_posting_lists_for_index removed - not needed with DSA-based system */

/*
 * Get or create a posting list for the given term string
 * Returns pointer to posting list, creating it if necessary
 * Uses DSA-based string table functions for storage
 */

/*
 * Add terms from a document to the posting lists
 * This is the core operation for building the inverted index
 * Uses DSA-based string table functions for storage
 */
void
tp_add_document_terms(TpIndexState * index_state,
						ItemPointer ctid,
						char **terms,
						int32 *frequencies,
						int term_count,
						int32 doc_length)
{
	int i;

	elog(DEBUG1, "tp_add_document_terms called with index_state=%p, term_count=%d",
		 index_state, term_count);

	Assert(index_state != NULL);
	Assert(ctid != NULL);
	Assert(terms != NULL);
	Assert(frequencies != NULL);

	/* Process each term */
	for (i = 0; i < term_count; i++)
	{
		char *term = terms[i];
		int32 frequency = frequencies[i];
		TpPostingList *posting_list;

		if (!term || frequency <= 0)
			continue;

		/* Get or create posting list for this term through string interning */
		posting_list = tp_get_or_create_posting_list(index_state, term);
		if (!posting_list) {
			elog(WARNING, "Failed to get/create posting list for term '%s'", term);
			continue;
		}

		/* Add document entry to posting list */
		tp_add_document_to_posting_list(index_state, posting_list, ctid, frequency);
	}

	/* Store document length in hash table (once per document, not per term) */
	if (index_state->doc_lengths_hash)
	{
		TpDocLengthEntry doc_entry;
		TpDocLengthEntry *existing_entry;
		bool found;

		/* Set up the entry */
		doc_entry.ctid = *ctid;
		doc_entry.doc_length = doc_length;

		/* Insert into hash table */
		LWLockAcquire(&index_state->doc_lengths_lock, LW_EXCLUSIVE);
		existing_entry = (TpDocLengthEntry *) dshash_find_or_insert(
			index_state->doc_lengths_hash, &doc_entry.ctid, &found);

		if (!found)
		{
			/* New document - store the length */
			existing_entry->doc_length = doc_length;
			elog(DEBUG2, "Stored document length %d for ctid (%u,%u)",
				 doc_length, ItemPointerGetBlockNumber(ctid), ItemPointerGetOffsetNumber(ctid));
		}
		else
		{
			elog(DEBUG2, "Document ctid (%u,%u) already has length %d in hash table",
				 ItemPointerGetBlockNumber(ctid), ItemPointerGetOffsetNumber(ctid),
				 existing_entry->doc_length);
		}

		LWLockRelease(&index_state->doc_lengths_lock);
	}
	else
	{
		elog(DEBUG1, "Document length hash table not initialized for storing ctid (%u,%u)",
			 ItemPointerGetBlockNumber(ctid), ItemPointerGetOffsetNumber(ctid));
	}

	/* Update corpus statistics */
	LWLockAcquire(&index_state->corpus_stats_lock, LW_EXCLUSIVE);
	index_state->stats.total_docs++;
	index_state->stats.total_terms += term_count;
	elog(DEBUG2, "Updating corpus stats: index_state=%p, total_docs=%d, total_terms=%ld (added %d terms)",
		 index_state, index_state->stats.total_docs, index_state->stats.total_terms, term_count);

	/* Update document length statistics */
	index_state->stats.total_len += doc_length;
	elog(DEBUG1, "Updated document length: total_len=%ld (added %d), new avg_len=%.3f",
		 index_state->stats.total_len, doc_length,
		 (float)index_state->stats.total_len / index_state->stats.total_docs);

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

	elog(DEBUG1, "Added document with %d terms to DSA posting lists", term_count);
}

/* Legacy posting list code removed - using DSA-based system */

/*
 * Get posting list for a specific term
 * Returns NULL if term not found
 * Uses DSA-based string interning table
 */
TpPostingList *
tp_get_posting_list(TpIndexState * index_state,
					  const char *term)
{
	TpStringHashTable *string_table;
	TpStringHashEntry *string_entry;
	TpPostingList *posting_list;
	dsa_area   *area;
	size_t		term_len;

	Assert(index_state != NULL);

	if (!term)
	{
		elog(ERROR, "tp_get_posting_list: term is NULL");
		return NULL;
	}

	elog(DEBUG2, "tp_get_posting_list: searching for term='%s'", term);

	/* Get DSA area for this index using cached approach */
	area = tp_get_dsa_area_for_index(index_state, InvalidOid);
	if (!area)
	{
		elog(WARNING, "Cannot get DSA area for index");
		return NULL;
	}

	/* Get the string hash table from the index state */
	if (index_state->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		elog(DEBUG2, "String hash table not initialized for index");
		return NULL;
	}

	string_table = tp_hash_table_attach_dsa(area, index_state->string_hash_handle);
	if (!string_table)
	{
		elog(WARNING, "Failed to attach to string hash table");
		return NULL;
	}
	term_len = strlen(term);

	/* Look up the term in the string table */
	LWLockAcquire(&index_state->string_interning_lock, LW_SHARED);
	string_entry = tp_hash_lookup_dsa(area, string_table, term, term_len);

	if (string_entry && DsaPointerIsValid(string_entry->posting_list_dp))
	{
		posting_list = dsa_get_address(area, string_entry->posting_list_dp);
		elog(DEBUG2, "Found posting list for term '%s': doc_count=%d",
			 term, posting_list->doc_count);
		tp_hash_table_detach_dsa(string_table);
		LWLockRelease(&index_state->string_interning_lock);
		return posting_list;
	}
	else
	{
		tp_hash_table_detach_dsa(string_table);
		LWLockRelease(&index_state->string_interning_lock);
		elog(DEBUG2, "No posting list found for term '%s'", term);
		return NULL;
	}
}

/*
 * Get or create a posting list for a term
 * Uses DSA-based string interning table
 */
TpPostingList *
tp_get_or_create_posting_list(TpIndexState * index_state, const char *term)
{
	TpStringHashTable *string_table;
	TpStringHashEntry *string_entry;
	TpPostingList *posting_list;
	dsa_area   *area;
	dsa_pointer posting_list_dp;
	size_t		term_len;

	Assert(index_state != NULL);
	Assert(term != NULL);

	elog(DEBUG1, "tp_get_or_create_posting_list: term='%s', index_oid=%u, area_handle=%u",
		 term, index_state->index_oid, index_state->area_handle);

	term_len = strlen(term);

	/* Get DSA area for this index using cached approach */
	area = tp_get_dsa_area_for_index(index_state, InvalidOid);
	if (!area)
	{
		elog(ERROR, "Cannot get DSA area for index %u", index_state->index_oid);
		return NULL;
	}

	elog(DEBUG1, "tp_get_or_create_posting_list: Got DSA area %p for index %u", area, index_state->index_oid);

	/* Initialize string hash table if needed */
	LWLockAcquire(&index_state->string_interning_lock, LW_EXCLUSIVE);

	if (index_state->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		/* Create new dshash table */
		string_table = tp_hash_table_create_dsa(area, TP_DEFAULT_HASH_BUCKETS);
		if (!string_table)
		{
			LWLockRelease(&index_state->string_interning_lock);
			elog(ERROR, "Failed to create string hash table");
			return NULL;
		}

		/* Store the handle for other processes */
		index_state->string_hash_handle = tp_hash_table_get_handle(string_table);
		elog(DEBUG1, "Created string hash table for index with handle %lu", 
			 (unsigned long) index_state->string_hash_handle);
	}
	else
	{
		/* Attach to existing table */
		string_table = tp_hash_table_attach_dsa(area, index_state->string_hash_handle);
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
		elog(DEBUG2, "Inserted new term '%s' into string table", term);
	}

	/* Check if posting list already exists for this term */
	if (DsaPointerIsValid(string_entry->posting_list_dp))
	{
		posting_list = dsa_get_address(area, string_entry->posting_list_dp);
		elog(DEBUG2, "Found existing posting list for term '%s': doc_count=%d",
			 term, posting_list->doc_count);
	}
	else
	{
		/* Create new posting list */
		posting_list_dp = dsa_allocate(area, sizeof(TpPostingList));
		posting_list = dsa_get_address(area, posting_list_dp);

		/* Initialize posting list */
		memset(posting_list, 0, sizeof(TpPostingList));
		posting_list->doc_count = 0;
		posting_list->capacity = 0;
		posting_list->is_finalized = false;
		posting_list->doc_freq = 0;
		posting_list->entries_dp = InvalidDsaPointer;

		/* Associate posting list with string entry */
		string_entry->posting_list_dp = posting_list_dp;
		string_entry->doc_freq = 0;

		elog(DEBUG2, "Created new posting list for term '%s'", term);
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
tp_add_document_to_posting_list(TpIndexState *index_state,
								TpPostingList *posting_list,
								ItemPointer ctid,
								int32 frequency)
{
	TpPostingEntry *entries;
	TpPostingEntry *new_entry;
	dsa_area *area;
	dsa_pointer new_entries_dp;

	if (!posting_list || !index_state)
		return;

	/* Get DSA area from index state */
	area = tp_get_dsa_area_for_index(index_state, InvalidOid);
	if (!area)
	{
		elog(WARNING, "Cannot get DSA area for adding document to posting list");
		return;
	}

	/* Ensure we have capacity for the new entry */
	if (posting_list->doc_count >= posting_list->capacity)
	{
		/* Need to expand the entries array */
		int32 new_capacity = posting_list->capacity == 0 ? 8 : posting_list->capacity * 2;

		elog(DEBUG2, "Expanding posting list capacity from %d to %d",
			 posting_list->capacity, new_capacity);

		/* Allocate new array with expanded capacity */
		new_entries_dp = dsa_allocate(area, sizeof(TpPostingEntry) * new_capacity);
		if (!DsaPointerIsValid(new_entries_dp))
		{
			elog(WARNING, "Failed to allocate DSA memory for posting list expansion");
			return;
		}

		/* If we had existing entries, copy them to the new array */
		if (DsaPointerIsValid(posting_list->entries_dp) && posting_list->doc_count > 0)
		{
			TpPostingEntry *old_entries = dsa_get_address(area, posting_list->entries_dp);
			TpPostingEntry *new_entries = dsa_get_address(area, new_entries_dp);

			memcpy(new_entries, old_entries, sizeof(TpPostingEntry) * posting_list->doc_count);

			/* Free old array */
			dsa_free(area, posting_list->entries_dp);
		}

		/* Update posting list to use new array */
		posting_list->entries_dp = new_entries_dp;
		posting_list->capacity = new_capacity;
	}

	/* Initialize entries array if this is the first document */
	if (!DsaPointerIsValid(posting_list->entries_dp))
	{
		posting_list->entries_dp = dsa_allocate(area, sizeof(TpPostingEntry) * 8);
		posting_list->capacity = 8;
		if (!DsaPointerIsValid(posting_list->entries_dp))
		{
			elog(WARNING, "Failed to allocate initial DSA memory for posting list");
			return;
		}
	}

	/* Get the entries array and add the new document */
	entries = dsa_get_address(area, posting_list->entries_dp);
	new_entry = &entries[posting_list->doc_count];

	/* Store document information */
	new_entry->ctid = *ctid;
	new_entry->doc_id = posting_list->doc_count; /* Use doc_count as simple doc_id */
	new_entry->frequency = frequency;
	/* Note: doc_length now stored in separate hash table */

	/* Update posting list counters */
	posting_list->doc_count++;
	posting_list->doc_freq = posting_list->doc_count;

	elog(DEBUG2, "Added document ctid=(%u,%u) freq=%d to posting list: new doc_count=%d",
		 ItemPointerGetBlockNumber(ctid), ItemPointerGetOffsetNumber(ctid),
		 frequency, posting_list->doc_count);
}

/*
 * Centralized IDF calculation with epsilon flooring
 *
 * Calculates IDF using the standard BM25 formula: log((N - df + 0.5) / (df + 0.5))
 * If the result is negative or zero, applies epsilon flooring like Python BM25:
 * epsilon = 0.25 * average_idf
 */
float4
tp_calculate_idf(int32 doc_freq, int32 total_docs, float4 average_idf)
{
	double idf_numerator = (double)(total_docs - doc_freq + 0.5);
	double idf_denominator = (double)(doc_freq + 0.5);
	double idf_ratio = idf_numerator / idf_denominator;
	double raw_idf = log(idf_ratio);

	elog(DEBUG3, "TAPIR IDF: doc_freq=%d, total_docs=%d, average_idf=%.6f",
		 doc_freq, total_docs, average_idf);
	elog(DEBUG3, "TAPIR IDF: numerator=%.6f, denominator=%.6f, ratio=%.6f, raw_idf=%.6f",
		 idf_numerator, idf_denominator, idf_ratio, raw_idf);

	if (raw_idf <= 0.0)
	{
		/* Apply epsilon flooring like Python BM25: epsilon = 0.25 * average_idf */
		float4 epsilon = 0.25f * average_idf;
		elog(DEBUG3, "TAPIR IDF: EPSILON APPLIED (raw_idf <= 0.0), epsilon=%.6f", epsilon);
		return epsilon;
	}
	else
	{
		elog(DEBUG3, "TAPIR IDF: NO EPSILON (raw_idf > 0.0), returning raw_idf=%.6f", raw_idf);
		return (float4)raw_idf;
	}
}

/*
 * Calculate average IDF across all terms in the index
 * This is used for epsilon flooring in BM25 calculations to match Python BM25 behavior
 */
void
tp_calculate_average_idf(TpIndexState *index_state)
{
	TpStringHashTable *string_table;
	dsa_area *area;
	uint32 term_count = 0;
	double idf_sum = 0.0;
	int32 total_docs = index_state->stats.total_docs;

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

	string_table = tp_hash_table_attach_dsa(area, index_state->string_hash_handle);
	if (!string_table)
	{
		elog(WARNING, "Failed to attach to string hash table for IDF calculation");
		index_state->stats.average_idf = 0.0f;
		return;
	}

	/* Iterate through all entries using dshash sequential scan */
	{
		dshash_seq_status status;
		TpStringHashEntry *entry;
		
		dshash_seq_init(&status, string_table->dshash, false); /* shared lock */

		while ((entry = (TpStringHashEntry *) dshash_seq_next(&status)) != NULL)
		{
			/* Calculate IDF for this term if it has a posting list */
			if (DsaPointerIsValid(entry->posting_list_dp))
			{
				TpPostingList *posting_list = dsa_get_address(area, entry->posting_list_dp);
				int32 doc_freq = posting_list->doc_count;

				if (doc_freq > 0)
				{
					double idf_numerator = (double)(total_docs - doc_freq + 0.5);
					double idf_denominator = (double)(doc_freq + 0.5);
					double idf_ratio = idf_numerator / idf_denominator;
					double idf = log(idf_ratio);

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
		index_state->stats.average_idf = (float4)(idf_sum / (double)term_count);
	}
	else
	{
		index_state->stats.average_idf = 0.0f;
	}

	elog(DEBUG2, "Calculated average IDF: %.6f from %d terms",
		 index_state->stats.average_idf, term_count);
}

/*
 * Finalize index building by calculating corpus statistics
 * This converts from the building phase to query-ready phase
 * Uses DSA-based system for shared memory storage
 */
void
tp_finalize_index_build(TpIndexState * index_state)
{
	Assert(index_state != NULL);

	/* Calculate average IDF for epsilon flooring */
	tp_calculate_average_idf(index_state);

	elog(DEBUG1, "Tapir index build finalization completed");
	elog(DEBUG2, "Finalize: index_state=%p, DSM_state=%p, Total docs: %d, avg doc length: %.2f, average IDF: %.6f",
		 index_state, index_state, index_state->stats.total_docs,
		 index_state->stats.total_docs > 0 ?
		 (float4)(index_state->stats.total_len / (double)index_state->stats.total_docs) : 0.0f,
		 index_state->stats.average_idf);
}

/* bm25_score_document removed - replaced by more efficient tp_score_documents */

/*
 * Single-pass BM25 scoring for multiple documents
 * This is much more efficient than calling bm25_score_document for each document
 * Complexity: O(n × q) where n = total posting entries, q = query terms
 */
typedef struct DocumentScoreEntry
{
	ItemPointerData ctid;		/* Document CTID (hash key) */
	float4		score;			/* Accumulated BM25 score */
	float4		doc_length;		/* Document length (for BM25 calculation) */
}			DocumentScoreEntry;

int
tp_score_documents(TpIndexState * index_state,
				   char **query_terms,
				   int32 *query_frequencies,
				   int query_term_count,
							   float4 k1,
							   float4 b,
							   int max_results,
							   ItemPointer result_ctids,
							   float4 **result_scores)
{
	HASHCTL		hash_ctl;

	elog(DEBUG2, "tp_score_documents called with %d query terms", query_term_count);
	HTAB	   *doc_scores_hash;
	DocumentScoreEntry *doc_entry;
	float4		avg_doc_len;
	int32		total_docs;
	bool		found;
	int			result_count = 0;
	DocumentScoreEntry **sorted_docs;
	int			i, j;
	int			hash_size;

	elog(DEBUG2, "tp_score_documents: max_results=%d, query_term_count=%d",
		 max_results, query_term_count);

	/* Validate inputs */
	if (!index_state) {
		elog(ERROR, "tp_score_documents: NULL index_state");
	}
	if (!query_terms) {
		elog(ERROR, "tp_score_documents: NULL query_terms");
	}
	if (!query_frequencies) {
		elog(ERROR, "tp_score_documents: NULL query_frequencies");
	}
	if (!result_ctids) {
		elog(ERROR, "tp_score_documents: NULL result_ctids");
	}
	if (!result_scores) {
		elog(ERROR, "tp_score_documents: NULL result_scores");
	}

	elog(DEBUG2, "tp_score_documents: accessing index_state->stats");
	total_docs = index_state->stats.total_docs;
	avg_doc_len = total_docs > 0 ?
		(float4)(index_state->stats.total_len / (double)total_docs) : 0.0f;
	elog(DEBUG2, "tp_score_documents: total_docs=%d, avg_doc_len=%f", total_docs, avg_doc_len);

	/* Create hash table for accumulating document scores */
	elog(DEBUG2, "tp_score_documents: preparing hash table");

	/* Ensure reasonable hash table size with upper bounds */
	hash_size = max_results * 2;
	if (total_docs > 0 && hash_size > total_docs * 2)
		hash_size = total_docs * 2;
	if (hash_size < 128)
		hash_size = 128;  /* Minimum reasonable size */
	if (hash_size > 10000)
		hash_size = 10000;  /* Maximum reasonable size to prevent memory issues */
	elog(DEBUG2, "tp_score_documents: hash_size=%d (limited for stability)", hash_size);

	/* Initialize hash control structure */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(DocumentScoreEntry);

	elog(DEBUG3, "tp_score_documents: keysize=%zu, entrysize=%zu",
		 hash_ctl.keysize, hash_ctl.entrysize);

	/* Create hash table */
	doc_scores_hash = hash_create("DocScores",
								  hash_size,
								  &hash_ctl,
								  HASH_ELEM | HASH_BLOBS);

	elog(DEBUG2, "tp_score_documents: hash table created");

	if (!doc_scores_hash)
		elog(ERROR, "Failed to create document scores hash table");

	/* Single pass through all posting lists */
	elog(DEBUG2, "tp_score_documents: processing %d query terms", query_term_count);
	for (int term_idx = 0; term_idx < query_term_count; term_idx++)
	{
		const char *term = query_terms[term_idx];
		TpPostingList *posting_list;
		TpPostingEntry *entries;
		float4		idf;

		elog(DEBUG3, "tp_score_documents: term %d: '%s'", term_idx, term);
		posting_list = tp_get_posting_list(index_state, term);

		if (!posting_list || posting_list->doc_count == 0) {
			elog(DEBUG3, "No posting list for term '%s'", term);
			continue;
		}

		/* Calculate IDF using centralized function with epsilon flooring */
		idf = tp_calculate_idf(posting_list->doc_count, total_docs, index_state->stats.average_idf);

		/* Process each document in this term's posting list */
		/* Get DSA area and retrieve entries */
		{
			dsa_area *area = tp_get_dsa_area_for_index(index_state, InvalidOid);
			if (!area) continue; /* Skip if can't get DSA area */
			entries = tp_get_posting_entries(area, posting_list);
			if (!entries) continue; /* Skip if no entries stored yet */
		}
		for (int doc_idx = 0; doc_idx < posting_list->doc_count; doc_idx++)
		{
			TpPostingEntry *entry = &entries[doc_idx];
			float4		tf = entry->frequency;
			float4		doc_len;
			float4		term_score;
			double		numerator_d, denominator_d;

			/* Look up document length from hash table */
			doc_len = (float4)tp_get_document_length(index_state, &entry->ctid);

			/* Validate TID */
			if (!ItemPointerIsValid(&entry->ctid))
				continue;

			/* Calculate BM25 term score contribution */
			numerator_d = (double)tf * ((double)k1 + 1.0);

			/* Avoid division by zero - if avg_doc_len is 0, use doc_len directly */
			if (avg_doc_len > 0.0f)
			{
				denominator_d = (double)tf + (double)k1 * (1.0 - (double)b + (double)b * ((double)doc_len / (double)avg_doc_len));
			}
			else
			{
				/* When avg_doc_len is 0 (no corpus stats), fall back to standard TF formula */
				denominator_d = (double)tf + (double)k1;
			}

			term_score = (float4)((double)idf * (numerator_d / denominator_d) * (double)query_frequencies[term_idx]);

		/* Debug NaN detection */
		if (isnan(term_score))
		{
			elog(LOG, "NaN detected in term_score calculation: idf=%f, numerator_d=%f, denominator_d=%f, query_freq=%d, tf=%f, doc_len=%f, avg_doc_len=%f, k1=%f, b=%f",
				 idf, numerator_d, denominator_d, query_frequencies[term_idx], tf, doc_len, avg_doc_len, k1, b);
		}


			/* Find or create document entry in hash table */
			doc_entry = (DocumentScoreEntry *) hash_search(doc_scores_hash,
														   &entry->ctid,
														   HASH_ENTER,
														   &found);

			if (!found)
			{
				/* New document - initialize */
				doc_entry->ctid = entry->ctid;
				doc_entry->score = term_score;	/* Positive BM25 score */
				doc_entry->doc_length = doc_len;
			}
			else
			{
				/* Existing document - accumulate score */
				doc_entry->score += term_score;	/* Positive BM25 score */
			}
		}
	}

	/* Extract and sort documents by score */
	/* First, count how many unique documents we have */
	result_count = hash_get_num_entries(doc_scores_hash);

	elog(DEBUG2, "tp_score_documents: hash has %d entries, max_results=%d",
		 result_count, max_results);

	if (result_count > max_results)
		result_count = max_results;

	/* Allocate array for sorting (even if result_count is 0) */
	sorted_docs = result_count > 0 ? palloc(result_count * sizeof(DocumentScoreEntry *)) : NULL;

	/* Extract documents from hash table using hash_seq_search */
	i = 0;
	if (result_count > 0)
	{
		HASH_SEQ_STATUS seq_status;
		DocumentScoreEntry *doc_entry;

		elog(DEBUG1, "tp_score_documents: calling hash_seq_init on DocScores table");
		hash_seq_init(&seq_status, doc_scores_hash);

		while ((doc_entry = (DocumentScoreEntry *) hash_seq_search(&seq_status)) != NULL && i < result_count)
		{
			sorted_docs[i++] = doc_entry;
		}

		/* If we hit the result_count limit while doc_entry was still non-NULL,
		 * we need to terminate the scan manually. If hash_seq_search returned NULL,
		 * the scan completed naturally and cleanup was done automatically. */
		if (doc_entry != NULL && i >= result_count)
			hash_seq_term(&seq_status);
	}

	result_count = i;			/* Actual count extracted */

	elog(DEBUG2, "tp_score_documents: extracted %d unique documents using posting list iteration", result_count);

	/* Sort documents by score (descending - higher scores first) */
	for (i = 1; i < result_count; i++)
	{
		DocumentScoreEntry *key = sorted_docs[i];
		j = i - 1;

		while (j >= 0 && sorted_docs[j]->score > key->score)
		{
			sorted_docs[j + 1] = sorted_docs[j];
			j--;
		}
		sorted_docs[j + 1] = key;
	}

	/* Copy sorted results to output arrays */
	if (result_count > 0)
	{
		*result_scores = palloc(result_count * sizeof(float4));
		for (i = 0; i < result_count; i++)
		{
			result_ctids[i] = sorted_docs[i]->ctid;
			(*result_scores)[i] = sorted_docs[i]->score;
		}
	}
	else
	{
		*result_scores = NULL;
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
tp_get_corpus_statistics(TpIndexState * index_state)
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
	int32 doc_length = 0;

	if (!index_state->doc_lengths_hash)
	{
		elog(DEBUG1, "Document length hash table not initialized");
		return 0;
	}

	LWLockAcquire(&index_state->doc_lengths_lock, LW_SHARED);
	entry = (TpDocLengthEntry *) dshash_find(index_state->doc_lengths_hash, ctid, false);
	if (entry)
	{
		doc_length = entry->doc_length;
		elog(DEBUG3, "Found document length %d for ctid (%u,%u)",
			 doc_length, ItemPointerGetBlockNumber(ctid), ItemPointerGetOffsetNumber(ctid));
	}
	else
	{
		elog(DEBUG2, "Document length not found for ctid (%u,%u)",
			 ItemPointerGetBlockNumber(ctid), ItemPointerGetOffsetNumber(ctid));
	}
	LWLockRelease(&index_state->doc_lengths_lock);

	return doc_length;
}

/* grow_posting_list removed - growth handled directly in tp_add_document_to_posting_list */
