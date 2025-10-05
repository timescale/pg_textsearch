/*-------------------------------------------------------------------------
 *
 * state.c
 *	  Index state management for Tapir
 *
 * This module manages the TpLocalIndexState and TpSharedIndexState
 * structures, providing functions to get, create, and release index states.
 *
 * IDENTIFICATION
 *	  src/state.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/genam.h>
#include <access/heapam.h>
#include <access/relation.h>
#include <lib/dshash.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/dsm.h>
#include <storage/dsm_registry.h>
#include <utils/builtins.h>
#include <utils/dsa.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>

#include "index.h"
#include "metapage.h"
#include "posting.h"
#include "registry.h"
#include "state.h"
#include "stringtable.h"

/* Cache of local index states */
static HTAB *local_state_cache = NULL;

typedef struct LocalStateCacheEntry
{
	Oid				   index_oid;	/* Hash key */
	TpLocalIndexState *local_state; /* Cached local state */
} LocalStateCacheEntry;

/*
 * Initialize the local state cache
 */
static void
init_local_state_cache(void)
{
	HASHCTL ctl;

	if (local_state_cache != NULL)
		return;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize	  = sizeof(Oid);
	ctl.entrysize = sizeof(LocalStateCacheEntry);
	ctl.hcxt	  = TopMemoryContext;

	local_state_cache = hash_create(
			"Tapir Local State Cache",
			8, /* initial size */
			&ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Clear all cached local states
 * This is called when the DSA is being detached to prevent stale pointers
 */
void
tp_clear_all_local_states(void)
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;

	if (local_state_cache == NULL)
		return;

	/* Iterate through all cached entries */
	hash_seq_init(&status, local_state_cache);
	while ((entry = (LocalStateCacheEntry *)hash_seq_search(&status)) != NULL)
	{
		TpLocalIndexState *local_state = entry->local_state;
		if (local_state != NULL)
		{
			/* Note: We don't detach from DSA here since it's already being
			 * detached globally */
			local_state->dsa = NULL;
			pfree(local_state);
		}
	}

	/* Clear the entire cache */
	hash_destroy(local_state_cache);
	local_state_cache = NULL;
}

/*
 * Get or create a local index state for the given index OID
 *
 * This function:
 * 1. Checks if we already have a cached local state
 * 2. If not, looks up the shared state in the registry
 * 3. Attaches to the shared DSA if needed
 * 4. Creates and caches the local state
 */
TpLocalIndexState *
tp_get_local_index_state(Oid index_oid)
{
	LocalStateCacheEntry *entry;
	TpLocalIndexState	 *local_state;
	TpSharedIndexState	 *shared_state;
	bool				  found;
	dsa_area			 *dsa;

	/* Initialize cache if needed */
	init_local_state_cache();

	/* Check cache first */
	entry = (LocalStateCacheEntry *)
			hash_search(local_state_cache, &index_oid, HASH_FIND, NULL);

	if (entry != NULL)
		return entry->local_state;

	/* Look up shared state in registry */
	shared_state = tp_registry_lookup(index_oid);

	if (shared_state == NULL)
	{
		/*
		 * No registry entry found. This could mean:
		 * 1. The index was just dropped
		 * 2. We're in crash recovery after a restart
		 * 3. The index doesn't exist
		 * 4. The index is being built right now
		 *
		 * IMPORTANT: We should NEVER attempt to rebuild from disk during
		 * normal CREATE INDEX operations. The registry entry should be
		 * created BEFORE tp_insert is called during index build.
		 *
		 * Only attempt recovery during actual PostgreSQL crash recovery.
		 */

		/* Check if we're in actual crash recovery mode */
		if (!IsNormalProcessingMode() && !IsBootstrapProcessingMode())
		{
			/* We might be in recovery - check if index exists */
			Relation index_rel;
			bool	 index_exists = false;

			PG_TRY();
			{
				index_rel = index_open(index_oid, AccessShareLock);
				if (index_rel != NULL)
				{
					index_exists = true;
					index_close(index_rel, AccessShareLock);
				}
			}
			PG_CATCH();
			{
				/* Index doesn't exist - that's fine */
				FlushErrorState();
			}
			PG_END_TRY();

			if (index_exists)
			{
				/* We're in recovery mode - carefully try to rebuild from disk
				 */
				elog(INFO,
					 "Index %u exists on disk but not in registry - "
					 "attempting recovery",
					 index_oid);

				/* Only attempt recovery if we can safely validate the metapage
				 */
				local_state = tp_rebuild_index_from_disk(index_oid);
				if (local_state != NULL)
				{
					return local_state;
				}

				/* Recovery failed - index might be corrupted or stale */
			}
		}

		/* Index not found in registry and we're not in recovery mode */
		return NULL;
	}

	/* If we reach here with shared_state set, it's actually a DSA pointer */
	if (shared_state != NULL)
	{
		/* shared_state is actually a DSA pointer - need to attach to DSA and
		 * convert to address */
		dsa_pointer shared_dp = (dsa_pointer)(uintptr_t)shared_state;

		/* Get the shared DSA area */
		dsa = tp_registry_get_dsa();

		/* Convert DSA pointer to memory address in this backend */
		shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);

		/* Allocate local state */
		local_state = (TpLocalIndexState *)MemoryContextAlloc(
				TopMemoryContext, sizeof(TpLocalIndexState));
		local_state->shared = shared_state;
		local_state->dsa	= dsa;

		/* Cache the local state */
		entry = (LocalStateCacheEntry *)
				hash_search(local_state_cache, &index_oid, HASH_ENTER, &found);
		entry->local_state = local_state;

		return local_state;
	}

	return NULL;
}

/*
 * Release a local index state
 *
 * This detaches from the DSA area and removes the entry from the cache
 */
void
tp_release_local_index_state(TpLocalIndexState *local_state)
{
	bool found;
	Oid	 index_oid;

	if (local_state == NULL)
		return;

	/* Save index OID before accessing shared state */
	if (local_state->shared != NULL)
		index_oid = local_state->shared->index_oid;
	else
		index_oid = InvalidOid;

	/* Remove from cache */
	if (local_state_cache != NULL && OidIsValid(index_oid))
	{
		hash_search(local_state_cache, &index_oid, HASH_REMOVE, &found);
	}

	/* Detach from DSA */
	if (local_state->dsa != NULL)
		dsa_detach(local_state->dsa);

	/* Free the local state */
	pfree(local_state);
}

/*
 * Create a new shared index state and return local state
 *
 * This is called during CREATE INDEX to set up the initial shared state
 * and return a ready-to-use local state to avoid double DSA attachment
 */
TpLocalIndexState *
tp_create_shared_index_state(Oid index_oid, Oid heap_oid)
{
	TpSharedIndexState	 *shared_state;
	TpLocalIndexState	 *local_state;
	TpMemtable			 *memtable;
	dsa_area			 *dsa;
	dsa_pointer			  shared_dp;
	dsa_pointer			  memtable_dp;
	LocalStateCacheEntry *entry;
	bool				  found;

	/* Get the shared DSA area */
	dsa = tp_registry_get_dsa();

	/* Allocate shared state in DSA */
	shared_dp = dsa_allocate(dsa, sizeof(TpSharedIndexState));
	if (shared_dp == InvalidDsaPointer)
	{
		elog(ERROR, "Failed to allocate DSA memory for shared state");
	}
	shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);

	/* Initialize shared state */
	shared_state->index_oid	 = index_oid;
	shared_state->heap_oid	 = heap_oid;
	shared_state->total_docs = 0;
	shared_state->total_len	 = 0;
	shared_state->idf_sum	 = 0.0;

	/* Allocate and initialize memtable */
	memtable_dp = dsa_allocate(dsa, sizeof(TpMemtable));

	memtable = (TpMemtable *)dsa_get_address(dsa, memtable_dp);
	memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	memtable->total_terms		 = 0;
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;

	shared_state->memtable_dp = memtable_dp;

	/* Register in global registry */
	if (!tp_registry_register(index_oid, shared_state, shared_dp))
	{
		/* If registration failed, try initializing the registry first */
		tp_registry_shmem_startup();
		if (!tp_registry_register(index_oid, shared_state, shared_dp))
		{
			/* Clean up allocations on failure */
			dsa_free(dsa, memtable_dp);
			dsa_free(dsa, shared_dp);
			elog(ERROR, "Failed to register index %u", index_oid);
		}
	}

	/* Create local state for the creating backend */
	local_state = (TpLocalIndexState *)
			MemoryContextAlloc(TopMemoryContext, sizeof(TpLocalIndexState));
	local_state->shared = shared_state;
	local_state->dsa	= dsa;

	/* Cache the local state */
	init_local_state_cache();
	entry = (LocalStateCacheEntry *)
			hash_search(local_state_cache, &index_oid, HASH_ENTER, &found);
	if (found)
		elog(ERROR,
			 "Local state cache entry already exists for index %u",
			 index_oid);

	entry->local_state = local_state;

	return local_state;
}

/*
 * Clean up shared memory allocations for an index
 *
 * This is called when an index is dropped. We free the DSA allocations
 * but keep the DSA area itself since it's shared by all indices.
 */
void
tp_cleanup_index_shared_memory(Oid index_oid)
{
	dsa_area			 *dsa;
	dsa_pointer			  shared_dp;
	TpSharedIndexState	 *shared_state;
	TpMemtable			 *memtable;
	LocalStateCacheEntry *entry = NULL;
	bool				  found;

	/* Look up the DSA pointer in registry */
	shared_dp = tp_registry_lookup_dsa(index_oid);

	if (!DsaPointerIsValid(shared_dp))
	{
		/* Still unregister even if no shared state found */
		tp_registry_unregister(index_oid);
		return; /* Nothing to clean up */
	}

	/* Get the shared DSA area */
	dsa = tp_registry_get_dsa();

	/* Get shared state to access memtable */
	shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);
	memtable = (TpMemtable *)dsa_get_address(dsa, shared_state->memtable_dp);

	/* Destroy the string hash table if it exists */
	if (memtable->string_hash_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_table *string_hash;

		string_hash =
				tp_string_table_attach(dsa, memtable->string_hash_handle);
		if (string_hash != NULL)
			dshash_destroy(string_hash);
	}

	/* Destroy the document lengths hash table if it exists */
	if (memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_table *doc_lengths_hash;

		doc_lengths_hash =
				tp_doclength_table_attach(dsa, memtable->doc_lengths_handle);
		if (doc_lengths_hash != NULL)
			dshash_destroy(doc_lengths_hash);
	}

	/* Free shared state structures from DSA */
	dsa_free(dsa, shared_state->memtable_dp);
	dsa_free(dsa, shared_dp);

	/* Clean up local state if we have it cached */
	if (local_state_cache != NULL)
	{
		entry = hash_search(local_state_cache, &index_oid, HASH_FIND, &found);
		if (found && entry != NULL && entry->local_state != NULL)
		{
			/* Remove from cache first, before detaching DSA */
			hash_search(local_state_cache, &index_oid, HASH_REMOVE, &found);

			/* Don't detach DSA - it's shared and still in use by registry */
			/* Just nullify the reference */
			entry->local_state->dsa	   = NULL;
			entry->local_state->shared = NULL;

			/* Free the local state */
			pfree(entry->local_state);
		}
	}

	/* Unregister from global registry AFTER cleanup */
	tp_registry_unregister(index_oid);
}

/*
 * Get local index state from cache without creating it if not found
 * Returns NULL if the index is not in the local cache
 */
TpLocalIndexState *
tp_get_local_index_state_if_cached(Oid index_oid)
{
	LocalStateCacheEntry *entry;
	bool				  found;

	/* Ensure the cache is initialized */
	if (local_state_cache == NULL)
		return NULL;

	/* Look up in local cache */
	entry = (LocalStateCacheEntry *)
			hash_search(local_state_cache, &index_oid, HASH_FIND, &found);

	if (!found)
		return NULL;

	return entry->local_state;
}

/*
 * Destroy a shared index state
 *
 * This is called during DROP INDEX to clean up the shared state
 */
void
tp_destroy_shared_index_state(TpSharedIndexState *shared_state)
{
	if (shared_state == NULL)
		return;

	/* Use the common cleanup function */
	tp_cleanup_index_shared_memory(shared_state->index_oid);
}

/*
 * Rebuild index state from disk after PostgreSQL restart
 * This recreates the DSA area and shared state from docid pages
 */
TpLocalIndexState *
tp_rebuild_index_from_disk(Oid index_oid)
{
	Relation		   index_rel;
	TpIndexMetaPage	   metap;
	TpLocalIndexState *local_state;

	/* Open the index relation */
	index_rel = index_open(index_oid, AccessShareLock);
	if (index_rel == NULL)
	{
		elog(WARNING, "Could not open index %u for recovery", index_oid);
		return NULL;
	}

	/* Get metapage */
	metap = tp_get_metapage(index_rel);
	if (metap == NULL)
	{
		index_close(index_rel, AccessShareLock);
		elog(WARNING, "Could not read metapage for index %u", index_oid);
		return NULL;
	}

	/* Validate that this is actually our metapage and not stale data */
	if (metap->magic != TP_MAGIC)
	{
		index_close(index_rel, AccessShareLock);
		pfree(metap);
		elog(WARNING,
			 "Invalid magic number in metapage for index %u: expected 0x%08X, "
			 "found 0x%08X",
			 index_oid,
			 TP_MAGIC,
			 metap->magic);
		return NULL;
	}

	/* Check if there's actually anything to recover */
	if (metap->total_docs == 0 &&
		metap->first_docid_page == InvalidBlockNumber)
	{
		/* Empty index - nothing to recover */
		index_close(index_rel, AccessShareLock);
		pfree(metap);

		/* Still create the shared state for the empty index */
		return tp_create_shared_index_state(
				index_oid, index_rel->rd_index->indrelid);
	}

	/* Create fresh state first */
	/* Creating fresh state for restart recovery */
	local_state = tp_create_shared_index_state(
			index_oid, index_rel->rd_index->indrelid);

	if (local_state != NULL)
	{
		/* Rebuild posting lists from docid pages */
		tp_rebuild_posting_lists_from_docids(index_rel, local_state, metap);

		/* Recalculate IDF sum after recovery for proper BM25 scoring */
		tp_calculate_idf_sum(local_state);
	}

	/* Clean up */
	if (metap)
		pfree(metap);
	index_close(index_rel, AccessShareLock);

	return local_state;
}

/*
 * Rebuild posting lists from docid pages stored on disk
 * This scans the docid pages, retrieves documents from heap, and rebuilds the
 * posting lists
 */
void
tp_rebuild_posting_lists_from_docids(
		Relation		   index_rel,
		TpLocalIndexState *local_state,
		TpIndexMetaPage	   metap)
{
	Buffer			   docid_buf;
	Page			   docid_page;
	TpDocidPageHeader *docid_header;
	ItemPointer		   docids;
	BlockNumber		   current_page;
	Relation		   heap_rel;

	if (!metap || metap->first_docid_page == InvalidBlockNumber)
	{
		return;
	}

	/* Inform user that recovery is starting */
	elog(INFO,
		 "Recovering pg_textsearch index %u from disk",
		 index_rel->rd_id);

	/* Open the heap relation to fetch document text */
	heap_rel = relation_open(index_rel->rd_index->indrelid, AccessShareLock);

	current_page = metap->first_docid_page;

	/* Scan through all docid pages */
	while (current_page != InvalidBlockNumber)
	{
		/* Read the docid page */
		docid_buf = ReadBuffer(index_rel, current_page);
		LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
		docid_page	 = BufferGetPage(docid_buf);
		docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);

		/* Validate this is actually a docid page and not stale data */
		if (docid_header->magic != TP_DOCID_PAGE_MAGIC)
		{
			UnlockReleaseBuffer(docid_buf);
			elog(WARNING,
				 "Invalid docid page magic at block %u: expected 0x%08X, "
				 "found 0x%08X - stopping recovery",
				 current_page,
				 TP_DOCID_PAGE_MAGIC,
				 docid_header->magic);
			break; /* Stop recovery - we've hit invalid/stale data */
		}

		/* Validate num_docids is reasonable - use a hardcoded max since we
		 * don't have TP_DOCIDS_PER_PAGE available here */
		if (docid_header->num_docids > 1000) /* Conservative upper bound */
		{
			UnlockReleaseBuffer(docid_buf);
			elog(WARNING,
				 "Invalid docid count at block %u: %d (max 1000) - stopping "
				 "recovery",
				 current_page,
				 docid_header->num_docids);
			break; /* Stop recovery - corrupted header */
		}

		/* Get docids array with proper alignment */
		docids = (ItemPointer)((char *)docid_header +
							   MAXALIGN(sizeof(TpDocidPageHeader)));

		/* Process each docid on this page */
		for (unsigned int i = 0; i < docid_header->num_docids; i++)
		{
			ItemPointer	  ctid = &docids[i];
			HeapTupleData tuple_data;
			HeapTuple	  tuple = &tuple_data;
			Buffer		  heap_buf;
			bool		  valid;
			Datum		  text_datum;
			bool		  isnull;
			text		 *document_text;
			int32		  doc_length;

			/* Validate the ItemPointer before attempting fetch */
			if (!ItemPointerIsValid(ctid))
			{
				elog(WARNING, "Invalid ItemPointer in docid page - skipping");
				continue;
			}

			/* Additional sanity check on the pointer values */
			if (ItemPointerGetBlockNumber(ctid) == InvalidBlockNumber ||
				ItemPointerGetOffsetNumber(ctid) == 0 ||
				ItemPointerGetOffsetNumber(ctid) > MaxHeapTuplesPerPage)
			{
				elog(WARNING,
					 "Suspicious ItemPointer (%u,%u) in docid page - skipping",
					 ItemPointerGetBlockNumber(ctid),
					 ItemPointerGetOffsetNumber(ctid));
				continue;
			}

			/* Initialize tuple for heap_fetch */
			tuple->t_self = *ctid;

			/* Fetch document from heap using the stored ctid */
			valid = heap_fetch(heap_rel, SnapshotAny, tuple, &heap_buf, true);
			if (!valid || !HeapTupleIsValid(tuple))
			{
				if (heap_buf != InvalidBuffer)
					ReleaseBuffer(heap_buf);
				continue; /* Skip invalid documents */
			}

			/* Extract text from the indexed column.
			 * We need to get the actual column number from the index.
			 * rd_index->indkey.values[0] contains the attribute number
			 * of the first indexed column in the heap relation.
			 */
			AttrNumber attnum = index_rel->rd_index->indkey.values[0];
			text_datum		  = heap_getattr(
					   tuple, attnum, RelationGetDescr(heap_rel), &isnull);

			if (!isnull)
			{
				document_text = DatumGetTextPP(text_datum);

				/* Use shared helper to process document text and rebuild
				 * posting lists */
				if (tp_process_document_text(
							document_text,
							ctid,
							metap->text_config_oid,
							local_state,
							&doc_length))
				{
					/* Update corpus statistics */
					local_state->shared->total_docs++;
					local_state->shared->total_len += doc_length;
				}
			}

			ReleaseBuffer(heap_buf);
		}

		/* Move to next page */
		current_page = docid_header->next_page;

		UnlockReleaseBuffer(docid_buf);
	}

	relation_close(heap_rel, AccessShareLock);

	/* Log recovery completion */
	if (local_state && local_state->shared)
	{
		elog(INFO,
			 "Recovery complete for tapir index %u: %u documents restored",
			 index_rel->rd_id,
			 local_state->shared->total_docs);
	}
}

/*
 * Helper function to get memtable from local index state
 * Canonical implementation used by all modules
 */
TpMemtable *
get_memtable(TpLocalIndexState *local_state)
{
	if (!local_state || !local_state->shared || !local_state->dsa)
		return NULL;

	if (!DsaPointerIsValid(local_state->shared->memtable_dp))
		return NULL;

	return (TpMemtable *)dsa_get_address(
			local_state->dsa, local_state->shared->memtable_dp);
}
