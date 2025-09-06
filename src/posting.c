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
#include "index.h"
#include "vector.h"
#include "postgres.h"
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
#include "utils/elog.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
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

	/* Initialize corpus statistics from metapage */
	memset(&index_state->stats, 0, sizeof(TpCorpusStatistics));
	index_state->stats.last_checkpoint = GetCurrentTimestamp();
	
	/* Load stats from index metapage */
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
		
		/* Copy stats from metapage */
		index_state->stats.total_docs = meta->total_docs;
		index_state->stats.total_len = meta->total_len;
		
		/* Calculate derived stats */
		if (meta->total_docs > 0) {
			index_state->stats.max_doc_length = meta->total_len / (float4)meta->total_docs * 2.0; /* Estimate */
			index_state->stats.min_doc_length = meta->total_len / (float4)meta->total_docs * 0.5; /* Estimate */
		}
		
		/* Store k1 and b for BM25 scoring */
		index_state->stats.k1 = meta->k1;
		index_state->stats.b = meta->b;
		
		/* For v0.0a memtable-only implementation, posting lists are volatile.
		 * Mark if we need to rebuild from persisted document IDs. 
		 * Must access meta fields before releasing buffer! */
		index_state->first_docid_page = meta->first_docid_page;
		if (meta->first_docid_page != InvalidBlockNumber && 
			index_state->stats.total_docs > 0) {
			index_state->needs_rebuild = true;
			elog(DEBUG1, "Index %s needs posting list rebuild from %d persisted documents",
				 index_name, index_state->stats.total_docs);
		} else {
			index_state->needs_rebuild = false;
		}
		
		elog(DEBUG1, "Loaded stats from metapage for index %s: total_docs=%d, total_len=%ld, k1=%f, b=%f",
			 index_name, index_state->stats.total_docs, index_state->stats.total_len,
			 index_state->stats.k1, index_state->stats.b);
		
		UnlockReleaseBuffer(metabuf);
		
		index_close(index_rel, AccessShareLock);
	}

	/* Initialize counters */
	index_state->next_doc_id = 1;
	index_state->memory_used = sizeof(TpIndexState);
	index_state->memory_limit = (Size) tp_shared_memory_size * 1024 * 1024;
	index_state->total_posting_entries = 0;
	index_state->max_posting_entries = tp_calculate_max_posting_entries();
	index_state->is_finalized = false;

	/* For v0.0a memtable-only implementation, use the shared string table
	 * since TpIndexState is stored in shared memory */
	elog(DEBUG1, "About to assign shared string table %p to index %s", 
		 tp_string_hash_table, index_name);
	index_state->string_table = tp_string_hash_table;
	if (!index_state->string_table)
		elog(ERROR, "Shared string table not initialized for index %s", index_name);

	elog(DEBUG1, "Using shared string table %p for index %s", 
		 index_state->string_table, index_name);

	posting_shared_state->num_indexes++;

	LWLockRelease(posting_shared_state->index_registry_lock);

	elog(
		 LOG, "Created Tp index state for %s (OID %u) with string_table=%p", 
		 index_name, index_oid, index_state->string_table);
	return index_state;
}

/*
 * Rebuild posting lists from persisted document IDs
 * This is called when an index state needs its posting lists rebuilt
 * after PostgreSQL restart (for v0.0a memtable-only implementation)
 */
static void
tp_rebuild_posting_lists_for_index(TpIndexState *index_state)
{
	Relation	index_rel;
	Buffer		docid_buf;
	Page		docid_page;
	TpDocidPageHeader *docid_header;
	ItemPointer docids;
	BlockNumber current_page;
	
	if (!index_state || !index_state->needs_rebuild)
		return;
		
	if (index_state->first_docid_page == InvalidBlockNumber)
		return;
	
	elog(DEBUG1, "Rebuilding posting lists for index %s from docid pages",
		 index_state->index_name);
	
	/* Open the index relation */
	index_rel = index_open(index_state->index_oid, AccessShareLock);
	
	/* Iterate through all docid pages */
	current_page = index_state->first_docid_page;
	while (current_page != InvalidBlockNumber)
	{
		docid_buf = ReadBuffer(index_rel, current_page);
		LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
		docid_page = BufferGetPage(docid_buf);
		docid_header = (TpDocidPageHeader *) PageGetContents(docid_page);
		
		/* Validate page magic */
		if (docid_header->magic != TP_DOCID_PAGE_MAGIC)
		{
			elog(WARNING, "Invalid docid page magic on block %u, skipping",
				 current_page);
			UnlockReleaseBuffer(docid_buf);
			break;
		}
		
		/* Process each docid on this page */
		docids = (ItemPointer) ((char *) docid_header + sizeof(TpDocidPageHeader));
		
		for (int i = 0; i < docid_header->num_docids; i++)
		{
			ItemPointer ctid = &docids[i];
			Relation	heap_rel;
			HeapTuple	tuple;
			Buffer		heap_buf;
			bool		valid;
			
			elog(DEBUG3, "Rebuilding posting for docid (%u,%u)",
				 ItemPointerGetBlockNumber(ctid),
				 ItemPointerGetOffsetNumber(ctid));
			
			/* Find the heap relation for this index */
			heap_rel = relation_open(index_rel->rd_index->indrelid, AccessShareLock);
			
			/* Initialize tuple for heap_fetch */
			tuple = &((HeapTupleData) {0});
			tuple->t_self = *ctid;
			
			/* Fetch the tuple from the heap */
			valid = heap_fetch(heap_rel, SnapshotAny, tuple, &heap_buf, true);
			
			if (valid && HeapTupleIsValid(tuple))
			{
				/* Extract the indexed column */
				Datum		column_value;
				bool		is_null;
				TupleDesc	tuple_desc = RelationGetDescr(heap_rel);
				
				/* Get the indexed column value */
				column_value = heap_getattr(tuple, 1, tuple_desc, &is_null);
				
				if (!is_null)
				{
					text *document_text;
					Datum vector_datum;
					TpVector *bm25vec;
					int term_count;
					char **terms;
					float4 *frequencies;
					int doc_length = 0;
					char *ptr;
					
					document_text = DatumGetTextPP(column_value);
					
					/* Vectorize the document */
					vector_datum = DirectFunctionCall2(
													   to_tpvector,
													   PointerGetDatum(document_text),
													   CStringGetTextDatum(index_state->index_name));
					bm25vec = (TpVector *) DatumGetPointer(vector_datum);
					
					/* Extract terms and frequencies from vector */
					term_count = TPVECTOR_ENTRY_COUNT(bm25vec);
					if (term_count > 0)
					{
						terms = palloc(term_count * sizeof(char*));
						frequencies = palloc(term_count * sizeof(float4));
						
						ptr = (char *) TPVECTOR_ENTRIES_PTR(bm25vec);
						for (int j = 0; j < term_count; j++)
						{
							TpVectorEntry *entry = (TpVectorEntry *) ptr;
							char *term_str = palloc(entry->lexeme_len + 1);
							memcpy(term_str, entry->lexeme, entry->lexeme_len);
							term_str[entry->lexeme_len] = '\0';
							
							terms[j] = term_str;
							frequencies[j] = entry->frequency;
							doc_length += entry->frequency;
							
							ptr += sizeof(TpVectorEntry) + MAXALIGN(entry->lexeme_len);
						}
						
						/* Add document terms to posting lists */
						tp_add_document_terms(index_state, ctid, terms,
											  frequencies, term_count, doc_length);
						
						/* Clean up */
						for (int j = 0; j < term_count; j++)
							pfree(terms[j]);
						pfree(terms);
						pfree(frequencies);
					}
					
					pfree(bm25vec);
				}
				
				ReleaseBuffer(heap_buf);
			}
			
			relation_close(heap_rel, AccessShareLock);
		}
		
		/* Move to next page */
		current_page = docid_header->next_page;
		UnlockReleaseBuffer(docid_buf);
	}
	
	index_close(index_rel, AccessShareLock);
	
	/* Mark as rebuilt */
	index_state->needs_rebuild = false;
	
	elog(DEBUG1, "Completed rebuilding posting lists for index %s",
		 index_state->index_name);
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
	
	elog(DEBUG1, "tp_get_or_create_posting_list called for term '%s'", term);
	elog(DEBUG1, "index_state=%p, string_table=%p", index_state, index_state->string_table);

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
	elog(DEBUG1, "term_len=%zu", term_len);

	/* Use transaction-level locking for string table access */
	elog(DEBUG1, "About to acquire string table lock (shared mode)");
	tp_acquire_string_table_lock(index_state->string_table, false);
	elog(DEBUG1, "Acquired string table lock");

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

	elog(DEBUG1, "tp_add_document_terms called with index_state=%p, term_count=%d", 
		 index_state, term_count);

	Assert(index_state != NULL);
	Assert(ctid != NULL);
	Assert(terms != NULL);
	Assert(frequencies != NULL);
	
	elog(DEBUG1, "Assertions passed, checking string table=%p", index_state->string_table);

	/* Check if adding this document would exceed posting list capacity */
	if (index_state->total_posting_entries + term_count > index_state->max_posting_entries)
		elog(ERROR, "Posting list capacity exceeded: would have %u entries, limit %u (memory budget per index: %dMB)",
			 index_state->total_posting_entries + term_count, index_state->max_posting_entries, tp_shared_memory_size);

	/* Assign unique document ID */
	SpinLockAcquire(&index_state->doc_id_mutex);
	doc_id = index_state->next_doc_id++;
	SpinLockRelease(&index_state->doc_id_mutex);

	/* Add terms to posting lists */
	elog(DEBUG1, "Starting to add %d terms to posting lists", term_count);
	for (i = 0; i < term_count; i++)
	{
		TpPostingList *posting_list;
		TpPostingEntry *new_entry;

		elog(DEBUG1, "Processing term %d: '%s'", i, terms[i]);
		
		/* Get or create posting list for this term */
		posting_list = tp_get_or_create_posting_list(index_state, terms[i]);
		elog(DEBUG1, "Got posting list=%p for term '%s'", posting_list, terms[i]);
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
	
	/* Check if we need to rebuild posting lists from persisted docids */
	if (index_state->needs_rebuild)
	{
		elog(DEBUG1, "Triggering posting list rebuild for index %s", index_state->index_name);
		tp_rebuild_posting_lists_for_index(index_state);
	}

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
	float4		avg_doc_len;
	int32		total_docs;
	bool		found;
	int			result_count = 0;
	HASH_SEQ_STATUS seq_status;
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
	
	/* Ensure reasonable hash table size */
	hash_size = max_results * 2;
	if (total_docs > 0 && hash_size > total_docs * 2)
		hash_size = total_docs * 2;
	if (hash_size < 128)
		hash_size = 128;  /* Minimum reasonable size */
	elog(DEBUG2, "tp_score_documents: hash_size=%d", hash_size);
	
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
		float4		idf;
		double idf_numerator, idf_denominator, idf_ratio;
		
		elog(DEBUG3, "tp_score_documents: term %d: '%s'", term_idx, term);
		posting_list = tp_get_posting_list(index_state, term);

		if (!posting_list || posting_list->doc_count == 0) {
			elog(DEBUG3, "No posting list for term '%s'", term);
			continue;
		}

		/* Calculate IDF once for this term */
		idf_numerator = (double)(total_docs - posting_list->doc_count + 0.5);
		idf_denominator = (double)(posting_list->doc_count + 0.5);
		idf_ratio = idf_numerator / idf_denominator;
		idf = (float4)log(idf_ratio);

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
	
	elog(DEBUG2, "tp_score_documents: hash has %d entries, max_results=%d", 
		 result_count, max_results);
	
	if (result_count > max_results)
		result_count = max_results;
	
	/* Handle case where no documents match */
	if (result_count == 0)
	{
		*result_scores = NULL;
		hash_destroy(doc_scores_hash);
		return 0;
	}

	/* Allocate array for sorting */
	sorted_docs = palloc(result_count * sizeof(DocumentScoreEntry *));

	/* Extract documents from hash table */
	i = 0;
	hash_seq_init(&seq_status, doc_scores_hash);
	PG_TRY();
	{
		while ((doc_entry = (DocumentScoreEntry *) hash_seq_search(&seq_status)) != NULL && i < result_count)
		{
			sorted_docs[i++] = doc_entry;
		}
	}
	PG_CATCH();
	{
		/* Ensure hash scan is properly terminated on exception */
		hash_seq_term(&seq_status);
		hash_destroy(doc_scores_hash);
		pfree(sorted_docs);
		PG_RE_THROW();
	}
	PG_END_TRY();
	
	/* Normal termination of hash scan */
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
