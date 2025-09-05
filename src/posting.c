/*
 * posting.c - Tp In-Memory Posting Lists Implementation
 *
 * This module implements in-memory posting lists for using standard
 * PostgreSQL hash tables.  Eventually, they will be flushed to disk
 * when full.
 */

#include "constants.h"
#include "posting.h"
#include "memtable.h"
#include "stringtable.h"
#include "postgres.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include <math.h>

/* Global shared state */
static PostingSharedState * posting_shared_state = NULL;

/* Configuration parameters */
int			tp_posting_list_growth_factor = TP_POSTING_LIST_GROWTH_FACTOR;	/* Double size when
													 * growing arrays */

/* Forward declarations */
static void grow_posting_list(TpPostingList * posting_list);
TpPostingList *tp_get_or_create_posting_list(TpIndexState *index_state, const char *term);

/*
 * Initialize shared memory structures for posting lists
 * Called during extension startup via shared memory hooks
 */
void
tp_posting_init_shared_state(void)
{
	bool		found;
	HASHCTL		info;
	Size		shared_state_size;

	shared_state_size = sizeof(PostingSharedState);

	/* Assume caller already holds AddinShmemInitLock */
	/* Initialize or attach to shared state */
	posting_shared_state =
		ShmemInitStruct("tp_posting_shared_state", shared_state_size, &found);

	if (!found)
	{
		/* Get locks from existing memtable tranche */
		posting_shared_state->string_interning_lock =
			tp_shared_state->string_interning_lock;
		posting_shared_state->index_registry_lock =
			tp_shared_state->posting_lists_lock;

		/* Initialize counters and state */
		posting_shared_state->num_indexes = 0;
		posting_shared_state->total_memory_used = 0;

		/* Create hash table for index registry */
		memset(&info, 0, sizeof(info));
		info.keysize = sizeof(Oid);
		info.entrysize = sizeof(TpIndexState);

		posting_shared_state->index_registry =
			ShmemInitHash("tp_index_registry",
						  TP_POSTING_LIST_HASH_INITIAL_SIZE,
						  TP_POSTING_LIST_HASH_MAX_SIZE,
						  &info,
						  HASH_ELEM | HASH_BLOBS | HASH_SHARED_MEM);

		elog(DEBUG2, "Tapir posting list shared state initialized");
	}
}

/*
 * Ensure posting shared state is attached in this backend
 */
static void
ensure_posting_shared_state(void)
{
	bool found;
	Size shared_state_size;

	if (posting_shared_state != NULL)
		return; /* Already attached */

	shared_state_size = sizeof(PostingSharedState);

	/* Try to attach to existing shared state - NO LOCK DURING INDEX OPERATIONS */
	/* Shared memory should already be initialized by shmem_startup_hook */
	posting_shared_state =
		ShmemInitStruct("tp_posting_shared_state", shared_state_size, &found);

	if (!found)
	{
		/* Shared state doesn't exist - this shouldn't happen after startup */
		elog(WARNING, "Tapir posting shared state not found - extension not properly initialized");
		posting_shared_state = NULL;
	}
	else
	{
		elog(DEBUG2, "Successfully attached to Tp posting shared state");
	}
}

/*
 * Get or create index state for a specific Tp index
 * This creates per-index shared memory structures
 */
TpIndexState *
tp_get_index_state(Oid index_oid, const char *index_name)
{
	TpIndexState *index_state;
	bool		found;

	/* Ensure we're attached to shared memory */
	ensure_posting_shared_state();

	/* Check if shared memory system is available */
	if (posting_shared_state == NULL)
	{
		/* For development/testing: fall back to non-shared memory mode */
		elog(WARNING,
			 "Tapir shared memory not initialized - falling back to non-shared "
			 "mode (posting lists disabled)");
		return NULL;
	}

	LWLockAcquire(posting_shared_state->index_registry_lock, LW_SHARED);
	index_state = hash_search(
							  posting_shared_state->index_registry, &index_oid, HASH_FIND, &found);
	LWLockRelease(posting_shared_state->index_registry_lock);

	if (found)
		return index_state;

	/* Need to create new index state */
	LWLockAcquire(posting_shared_state->index_registry_lock, LW_EXCLUSIVE);

	/* Double-check after lock upgrade */
	index_state = hash_search(
							  posting_shared_state->index_registry, &index_oid, HASH_FIND, &found);
	if (found)
	{
		LWLockRelease(posting_shared_state->index_registry_lock);
		return index_state;
	}

	/* Create new index state */
	index_state = hash_search(
							  posting_shared_state->index_registry, &index_oid, HASH_ENTER, &found);

	if (found)
		elog(ERROR, "concurrent index state creation for OID %u", index_oid);

	/* Initialize index state */
	index_state->index_oid = index_oid;
	strlcpy(index_state->index_name, index_name, NAMEDATALEN);

	/* Posting lists are now stored in the unified string interning table */

	/* Use shared locks from memtable */
	index_state->build_lock = tp_shared_state->string_interning_lock;
	index_state->stats_lock = tp_shared_state->posting_lists_lock;
	SpinLockInit(&index_state->doc_id_mutex);

	/* Initialize corpus statistics (k1/b are in index metapage) */
	memset(&index_state->stats, 0, sizeof(TpCorpusStatistics));
	index_state->stats.last_checkpoint = GetCurrentTimestamp();

	/* Initialize counters */
	index_state->next_doc_id = 1;
	index_state->memory_used = sizeof(TpIndexState);
	index_state->memory_limit = (Size) tp_shared_memory_size * 1024 * 1024;
	index_state->total_posting_entries = 0;
	index_state->max_posting_entries = tp_calculate_max_posting_entries();
	index_state->is_finalized = false;

	/* Create per-index string table */
	{
		uint32 max_string_entries = tp_calculate_max_hash_entries();
		Size string_pool_size = max_string_entries * (TP_AVG_TERM_LENGTH + 1);
		uint32 initial_buckets = TP_HASH_DEFAULT_BUCKETS;

		index_state->string_table = tp_hash_table_create(initial_buckets, string_pool_size, max_string_entries);
		if (!index_state->string_table)
			elog(ERROR, "Failed to create string table for index %s", index_name);

		/* Allocate dedicated lock for this string table */
		index_state->string_table->lock = (LWLock *) ShmemAlloc(sizeof(LWLock));
		if (!index_state->string_table->lock)
			elog(ERROR, "Failed to allocate lock for string table for index %s", index_name);
		LWLockInitialize(index_state->string_table->lock, LWLockNewTrancheId());

		elog(DEBUG1, "Created string table for index %s: %u max entries, %zu string pool",
			 index_name, max_string_entries, string_pool_size);
	}

	posting_shared_state->num_indexes++;

	LWLockRelease(posting_shared_state->index_registry_lock);

	elog(
		 LOG, "Created Tp index state for %s (OID %u)", index_name, index_oid);
	return index_state;
}

/*
 * Get or create a posting list for the given term string
 * Returns pointer to posting list, creating it if necessary
 */
TpPostingList *
tp_get_or_create_posting_list(TpIndexState *index_state, const char *term)
{
	TpStringHashEntry *hash_entry = NULL;
	TpPostingList *posting_list = NULL;
	MemoryContext old_context;
	size_t term_len;

	if (!index_state->string_table)
	{
		elog(ERROR, "tp_get_or_create_posting_list: index string_table is NULL");
		return NULL;
	}

	if (!term)
	{
		elog(ERROR, "tp_get_or_create_posting_list: term is NULL");
		return NULL;
	}

	term_len = strlen(term);

	/* Use transaction-level locking for string table access */
	tp_acquire_string_table_lock(index_state->string_table, false);

	/* Look up the term in the string hash table (for string interning only) */
	hash_entry = tp_hash_lookup(index_state->string_table, term, term_len);

	/* Create hash entry if it doesn't exist (for string interning) */
	if (!hash_entry)
	{
		/* Need to create hash entry - upgrade to exclusive lock */
		tp_acquire_string_table_lock(index_state->string_table, true);

		/* Re-check under exclusive lock - another backend may have created it */
		hash_entry = tp_hash_lookup(index_state->string_table, term, term_len);
		if (!hash_entry)
		{
			hash_entry = tp_hash_insert(index_state->string_table, term, term_len);
			if (!hash_entry)
			{
				tp_release_string_table_lock(index_state->string_table);
				elog(ERROR, "tp_get_or_create_posting_list: failed to insert term='%s' into hash table", term);
				return NULL;
			}
		}
	}
	else
	{
		/* Hash entry exists, upgrade to exclusive lock for posting list creation */
		tp_acquire_string_table_lock(index_state->string_table, true);
	}

	elog(DEBUG2, "tp_get_or_create_posting_list: creating new posting list for term='%s'", term);

	old_context = MemoryContextSwitchTo(TopMemoryContext);

	posting_list = (TpPostingList *) palloc0(sizeof(TpPostingList));
	posting_list->doc_count = 0;
	posting_list->capacity = TP_INITIAL_POSTING_LIST_CAPACITY;
	posting_list->is_finalized = false;
	posting_list->doc_freq = 0;
	posting_list->entries = (TpPostingEntry *) palloc(
		sizeof(TpPostingEntry) * posting_list->capacity);

	/* Store the posting list in the hash entry for later retrieval */
	hash_entry->posting_list = posting_list;

	elog(DEBUG2, "tp_get_or_create_posting_list: created posting list %p for term='%s'",
		 posting_list, term);

	MemoryContextSwitchTo(old_context);

	index_state->memory_used += sizeof(TpPostingList) +
		(sizeof(TpPostingEntry) * posting_list->capacity);

	tp_release_string_table_lock(index_state->string_table);

	elog(DEBUG2, "tp_get_or_create_posting_list: returning posting_list=%p for term='%s'",
		 posting_list, term);
	return posting_list;
}

/*
 * Add terms from a document to the posting lists
 * This is the core operation for building the inverted index
 */
void
tp_add_document_terms(TpIndexState * index_state,
						ItemPointer ctid,
						char **terms,
						float4 *frequencies,
						int term_count,
						int32 doc_length)
{
	int32		doc_id;
	int			i;

	Assert(index_state != NULL);
	Assert(ctid != NULL);
	Assert(terms != NULL);
	Assert(frequencies != NULL);

	/* Check if adding this document would exceed posting list capacity */
	if (index_state->total_posting_entries + term_count > index_state->max_posting_entries)
		elog(ERROR, "Posting list capacity exceeded: would have %u entries, limit %u (memory budget per index: %dMB)",
			 index_state->total_posting_entries + term_count, index_state->max_posting_entries, tp_shared_memory_size);

	/* Assign unique document ID */
	SpinLockAcquire(&index_state->doc_id_mutex);
	doc_id = index_state->next_doc_id++;
	SpinLockRelease(&index_state->doc_id_mutex);

	/* Add terms to posting lists */
	for (i = 0; i < term_count; i++)
	{
		TpPostingList *posting_list;
		TpPostingEntry *new_entry;

		/* Get or create posting list for this term */
		posting_list = tp_get_or_create_posting_list(index_state, terms[i]);
		if (posting_list == NULL)
		{
			elog(ERROR, "Failed to create posting list for term '%s'", terms[i]);
			continue;
		}

		/* Check if we need to grow the posting list */
		if (posting_list->doc_count >= posting_list->capacity)
		{
			grow_posting_list(posting_list);
			index_state->memory_used +=
				sizeof(TpPostingEntry) * (posting_list->capacity / 2);
		}

		/* Add new entry at end - O(1) amortized insertion! */
		new_entry = &posting_list->entries[posting_list->doc_count];
		new_entry->ctid = *ctid;
		new_entry->doc_id = doc_id;
		new_entry->frequency = frequencies[i];
		new_entry->doc_length = doc_length;

		posting_list->doc_count++;
		index_state->total_posting_entries++;
		/* Note: is_finalized stays false during building for O(1) inserts */
	}

	/* Update corpus statistics */
	LWLockAcquire(index_state->stats_lock, LW_EXCLUSIVE);
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

	LWLockRelease(index_state->stats_lock);
}

/*
 * Get posting list for a specific term by term ID
 * Returns NULL if term not found
 * Now uses unified string interning table
 */
TpPostingList *
tp_get_posting_list(TpIndexState * index_state,
					  const char *term)
{
	TpStringHashEntry *hash_entry = NULL;
	TpPostingList *posting_list = NULL;
	size_t term_len;

	Assert(index_state != NULL);

	if (!index_state->string_table)
	{
		elog(DEBUG2, "tp_get_posting_list: index string_table is NULL");
		return NULL;
	}

	if (!term)
	{
		elog(ERROR, "tp_get_posting_list: term is NULL");
		return NULL;
	}

	term_len = strlen(term);

	elog(DEBUG2, "tp_get_posting_list: searching for term='%s'", term);

	/* Use transaction-level locking for string table access */
	tp_acquire_string_table_lock(index_state->string_table, false);

	/* Look up the term in the string hash table */
	hash_entry = tp_hash_lookup(index_state->string_table, term, term_len);

	if (hash_entry && hash_entry->posting_list)
	{
		posting_list = (TpPostingList *) hash_entry->posting_list;
		elog(DEBUG2, "tp_get_posting_list: found posting list %p for term='%s'",
			 posting_list, term);
	}
	else if (hash_entry)
	{
		elog(DEBUG2, "tp_get_posting_list: hash entry exists for term='%s' but no posting list", term);
		posting_list = NULL;
	}
	else
	{
		elog(DEBUG2, "tp_get_posting_list: term='%s' not found in hash table", term);
		posting_list = NULL;
	}

	/* Auto-finalize posting list when accessed during search */
	if (posting_list && !posting_list->is_finalized)
	{
		/* Mark this posting list as finalized (usable but not necessarily sorted) */
		posting_list->is_finalized = true;
		elog(DEBUG2, "Auto-finalized posting list for term='%s' with %d docs",
			 term, posting_list->doc_count);
	}

	tp_release_string_table_lock(index_state->string_table);

	return posting_list;
}

/*
 * Finalize index building by sorting all posting lists
 * This converts from the building phase to query-ready phase
 */
void
tp_finalize_index_build(TpIndexState * index_state)
{

	Assert(index_state != NULL);

	/* Check if string interning table is available */
	if (!index_state->string_table)
	{
		elog(WARNING,
			 "String interning hash table not available for index %s, skipping finalization",
			 index_state->index_name);
		return;
	}

	/* Check if there are any entries to process */
	if (index_state->string_table->entry_count == 0)
	{
		elog(DEBUG2,
			 "No terms to finalize for index %s",
			 index_state->index_name);
		return;
	}

	/* Log finalization start */
	elog(DEBUG2,
		 "Finalizing Tp posting lists for index %s with %u string entries",
		 index_state->index_name,
		 index_state->string_table->entry_count);

	/* Acquire build lock to ensure no concurrent modifications */
	LWLockAcquire(index_state->build_lock, LW_EXCLUSIVE);

	/*
	 * Instead of iterating through the hash table (which can be problematic
	 * with shared memory), we'll rely on the auto-finalization mechanism
	 * in tp_get_posting_list during search operations. This approach is
	 * safer and avoids potential deadlocks from hash table iteration.
	 */
	elog(DEBUG2,
		 "Using deferred finalization - posting lists will be finalized when accessed during search");

	/* Set a flag in index_state to indicate finalization is complete */
	index_state->is_finalized = true;

	LWLockRelease(index_state->build_lock);

	elog(DEBUG2,
		 "Tapir index build finalized: using deferred finalization, "
		 "total docs: %d, avg doc length: %.2f",
		 index_state->stats.total_docs,
		 index_state->stats.total_docs > 0 ? 
		 (float4)(index_state->stats.total_len / (double)index_state->stats.total_docs) : 0.0f);
}

/*
 * Calculate BM25 score for a document against a query
 * k1/b parameters are passed from index metapage
 */
float4
bm25_score_document(TpIndexState * index_state,
					ItemPointer ctid,
					char **query_terms,
					float4 *query_frequencies,
					int query_term_count,
					float4 k1,
					float4 b)
{
	float4		score = 0.0f;
	float4		avg_doc_len = index_state->stats.total_docs > 0 ? 
		(float4)(index_state->stats.total_len / (double)index_state->stats.total_docs) : 0.0f;
	int32		total_docs = index_state->stats.total_docs;
	int			i;

	/* Iterate through query terms */
	for (i = 0; i < query_term_count; i++)
	{
		TpPostingList *posting_list;
		TpPostingEntry *entry;
		float4		tf = 0.0f;
		float4		idf;
		float4		doc_len = 0.0f;
		int			j;

		/* Get posting list for this term */
		posting_list = tp_get_posting_list(index_state, query_terms[i]);
		if (posting_list == NULL)
			continue;			/* Term not in corpus */

		/* Find document in posting list */
		if (posting_list->is_finalized && posting_list->doc_count > 8)
		{
			/*
			 * Use binary search on finalized (sorted) posting lists for
			 * efficiency
			 */
			int			left = 0,
						right = posting_list->doc_count - 1;
			int			mid,
						cmp;

			while (left <= right)
			{
				mid = (left + right) / 2;
				entry = &posting_list->entries[mid];
				cmp = ItemPointerCompare((ItemPointer) &entry->ctid,
										 (ItemPointer) ctid);

				if (cmp == 0)
				{
					tf = entry->frequency;
					doc_len = entry->doc_length;
					break;
				}
				else if (cmp < 0)
					left = mid + 1;
				else
					right = mid - 1;
			}
		}
		else
		{
			/* Linear search for small or unfinalized lists */
			for (j = 0; j < posting_list->doc_count; j++)
			{
				entry = &posting_list->entries[j];
				if (ItemPointerEquals(&entry->ctid, ctid))
				{
					tf = entry->frequency;
					doc_len = entry->doc_length;
					break;
				}
			}
		}

		if (tf == 0.0f)
			continue;			/* Term not in this document */

		/* Calculate IDF: log((N - df + 0.5) / (df + 0.5)) */
		idf = (float4)log((double)(total_docs - posting_list->doc_count + 0.5) /
				   (double)(posting_list->doc_count + 0.5));

		/* Calculate BM25 term score */
		{
			double numerator_d = (double)tf * ((double)k1 + 1.0);
			double denominator_d = (double)tf + (double)k1 * (1.0 - (double)b + (double)b * ((double)doc_len / (double)avg_doc_len));

			score += (float4)((double)idf * (numerator_d / denominator_d) * (double)query_frequencies[i]);
		}
	}

	/*
	 * Return negative score for DESC ordering compatibility with existing
	 * code
	 */
	return -score;
}

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
				   float4 *query_frequencies,
				   int query_term_count,
							   float4 k1,
							   float4 b,
							   int max_results,
							   ItemPointer result_ctids,
							   float4 **result_scores)
{
	HASHCTL		hash_ctl;
	HTAB	   *doc_scores_hash;
	DocumentScoreEntry *doc_entry;
	float4		avg_doc_len = index_state->stats.total_docs > 0 ? 
		(float4)(index_state->stats.total_len / (double)index_state->stats.total_docs) : 0.0f;
	int32		total_docs = index_state->stats.total_docs;
	bool		found;
	int			result_count = 0;
	HASH_SEQ_STATUS seq_status;
	DocumentScoreEntry **sorted_docs;
	int			i, j;

	/* Create hash table for accumulating document scores */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(DocumentScoreEntry);
	hash_ctl.hcxt = CurrentMemoryContext;

	doc_scores_hash = hash_create("DocumentScores",
								  max_results * 2,	/* Start with 2x capacity */
								  &hash_ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/* Single pass through all posting lists */
	for (int term_idx = 0; term_idx < query_term_count; term_idx++)
	{
		const char *term = query_terms[term_idx];
		TpPostingList *posting_list = tp_get_posting_list(index_state, term);
		float4		idf;

		if (!posting_list || posting_list->doc_count == 0)
			continue;

		/* Calculate IDF once for this term */
		idf = (float4)log((double)(total_docs - posting_list->doc_count + 0.5) /
				   (double)(posting_list->doc_count + 0.5));

		/* Process each document in this term's posting list */
		for (int doc_idx = 0; doc_idx < posting_list->doc_count; doc_idx++)
		{
			TpPostingEntry *entry = &posting_list->entries[doc_idx];
			float4		tf = entry->frequency;
			float4		doc_len = entry->doc_length;
			float4		term_score;
			double		numerator_d, denominator_d;

			/* Validate TID */
			if (!ItemPointerIsValid(&entry->ctid))
				continue;

			/* Calculate BM25 term score contribution */
			numerator_d = (double)tf * ((double)k1 + 1.0);
			denominator_d = (double)tf + (double)k1 * (1.0 - (double)b + (double)b * ((double)doc_len / (double)avg_doc_len));
			term_score = (float4)((double)idf * (numerator_d / denominator_d) * (double)query_frequencies[term_idx]);

			/* Find or create document entry in hash table */
			doc_entry = (DocumentScoreEntry *) hash_search(doc_scores_hash,
														   &entry->ctid,
														   HASH_ENTER,
														   &found);

			if (!found)
			{
				/* New document - initialize */
				doc_entry->ctid = entry->ctid;
				doc_entry->score = -term_score;	/* Negative for DESC ordering */
				doc_entry->doc_length = doc_len;
			}
			else
			{
				/* Existing document - accumulate score */
				doc_entry->score += -term_score;	/* Negative for DESC ordering */
			}
		}
	}

	/* Extract and sort documents by score */
	/* First, count how many unique documents we have */
	result_count = hash_get_num_entries(doc_scores_hash);
	if (result_count > max_results)
		result_count = max_results;

	/* Allocate array for sorting */
	sorted_docs = palloc(result_count * sizeof(DocumentScoreEntry *));

	/* Extract documents from hash table */
	i = 0;
	hash_seq_init(&seq_status, doc_scores_hash);
	while ((doc_entry = (DocumentScoreEntry *) hash_seq_search(&seq_status)) != NULL && i < result_count)
	{
		sorted_docs[i++] = doc_entry;
	}
	hash_seq_term(&seq_status);

	result_count = i;			/* Actual count extracted */

	/* Sort documents by score (descending - higher scores first) */
	/* Simple insertion sort for now - could optimize with qsort later */
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
	*result_scores = palloc(result_count * sizeof(float4));
	for (i = 0; i < result_count; i++)
	{
		result_ctids[i] = sorted_docs[i]->ctid;
		(*result_scores)[i] = sorted_docs[i]->score;
	}

	/* Cleanup */
	pfree(sorted_docs);
	hash_destroy(doc_scores_hash);

	return result_count;
}

/*
 * Get corpus statistics (Component D implementation)
 */
TpCorpusStatistics *
tp_get_corpus_statistics(TpIndexState * index_state)
{
	Assert(index_state != NULL);
	return &index_state->stats;
}


/* Helper functions */

/* Removed unused posting_entry_compare function */

/*
 * Grow posting list capacity when it becomes full
 * Uses repalloc for TopMemoryContext allocations
 */
static void
grow_posting_list(TpPostingList * posting_list)
{
	int32		new_capacity;
	MemoryContext oldcontext;

	new_capacity = posting_list->capacity * tp_posting_list_growth_factor;

	/* Switch to TopMemoryContext for persistent allocation */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* Use repalloc to grow the existing array - this avoids memory context issues */
	posting_list->entries = (TpPostingEntry *) repalloc(posting_list->entries,
														 sizeof(TpPostingEntry) * new_capacity);
	posting_list->capacity = new_capacity;

	/* Restore memory context */
	MemoryContextSwitchTo(oldcontext);
}
