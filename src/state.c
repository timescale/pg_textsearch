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
#include "registry.h"
#include "state.h"

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
 * Get or create a local index state for the given index OID
 *
 * This function:
 * 1. Checks if we already have a cached local state
 * 2. If not, looks up the shared state in the registry
 * 3. Creates a DSA area if needed and attaches to it
 * 4. Creates and caches the local state
 */
TpLocalIndexState *
tp_get_local_index_state(Oid index_oid)
{
	LocalStateCacheEntry *entry;
	TpLocalIndexState	 *local_state;
	TpSharedIndexState	 *shared_state;
	bool				  found;
	char				 *dsm_name;
	void				 *dsm_seg;
	size_t				  total_size;
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
		/* No registry entry - check if DSM segment exists */
		dsm_name   = psprintf("tapir.%u.%u", MyDatabaseId, index_oid);
		total_size = sizeof(TpDsmSegmentHeader);
		dsm_seg	   = GetNamedDSMSegment(dsm_name, total_size, NULL, &found);

		if (found)
		{
			/* DSM segment exists - check if it has valid contents */
			TpDsmSegmentHeader *dsm_header = (TpDsmSegmentHeader *)dsm_seg;

			elog(NOTICE,
				 "Recovery: DSM header check - handle: %u, shared_state_dp: "
				 "%lu",
				 dsm_header->dsm_handle,
				 (unsigned long)dsm_header->shared_state_dp);

			if (dsm_header->dsm_handle != DSM_HANDLE_INVALID &&
				DsaPointerIsValid(dsm_header->shared_state_dp))
			{
				/* DSM segment has valid contents - try to attach */
				elog(NOTICE,
					 "Recovery: attempting to attach to DSA handle %u for "
					 "index %u",
					 dsm_header->dsm_handle,
					 index_oid);
				/* Attach to DSA area in TopMemoryContext for persistence */
				{
					MemoryContext oldcontext = MemoryContextSwitchTo(
							TopMemoryContext);
					dsa = dsa_attach(dsm_header->dsm_handle);
					MemoryContextSwitchTo(oldcontext);
				}

				if (dsa == NULL)
				{
					elog(WARNING,
						 "Recovery: dsa_attach failed, rebuilding from disk");
					/* Fall through to disk rebuilding */
				}
				else
				{
					dsa_pin_mapping(dsa); /* Pin this backend's mapping */
					elog(NOTICE, "Recovery: DSA attachment successful");

					/* Get the existing shared state */
					shared_state = (TpSharedIndexState *)
							dsa_get_address(dsa, dsm_header->shared_state_dp);

					/* Register the recovered state */
					if (!tp_registry_register(index_oid, shared_state))
					{
						elog(WARNING,
							 "Failed to register recovered index %u",
							 index_oid);
						dsa_detach(dsa);
						pfree(dsm_name);
						return NULL;
					}

					elog(NOTICE,
						 "Recovered index state for index %u",
						 index_oid);
					/* Continue with normal local state creation below */
				}
			}
		}

		/* If we get here, either no DSM segment found or DSM contents invalid
		 * - rebuild from disk */
		if (shared_state == NULL)
		{
			/* Rebuilding from docid pages */

			/* Recover index state from metapage and docid pages */
			local_state = tp_rebuild_index_from_disk(index_oid);
			if (local_state != NULL)
			{
				/* Index state successfully rebuilt */
				pfree(dsm_name);
				return local_state;
			}

			pfree(dsm_name);
			return NULL; /* Recovery failed */
		}
	}

	/* If we reach here with shared_state set, it's actually a DSA pointer */
	if (shared_state != NULL)
	{
		/* shared_state is actually a DSA pointer - need to attach to DSA and
		 * convert to address */
		dsa_pointer shared_dp = (dsa_pointer)(uintptr_t)shared_state;

		/* Attach to the DSA area */
		dsm_name   = psprintf("tapir.%u.%u", MyDatabaseId, index_oid);
		total_size = sizeof(TpDsmSegmentHeader);
		dsm_seg	   = GetNamedDSMSegment(dsm_name, total_size, NULL, &found);

		if (!found || dsm_seg == NULL)
		{
			elog(ERROR, "DSM segment not found for index %u", index_oid);
			pfree(dsm_name);
			return NULL;
		}

		TpDsmSegmentHeader *dsm_header = (TpDsmSegmentHeader *)dsm_seg;

		/* Attach to DSA area in TopMemoryContext for persistence */
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
			dsa						 = dsa_attach(dsm_header->dsm_handle);
			MemoryContextSwitchTo(oldcontext);
		}

		if (dsa == NULL)
		{
			elog(ERROR, "Failed to attach to DSA for index %u", index_oid);
			pfree(dsm_name);
			return NULL;
		}

		/* Pin the mapping for cross-backend persistence */
		dsa_pin_mapping(dsa);

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

		pfree(dsm_name);
		return local_state;
	}

	pfree(dsm_name);
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

	if (local_state == NULL)
		return;

	/* Remove from cache */
	if (local_state_cache != NULL)
	{
		hash_search(
				local_state_cache,
				&local_state->shared->index_oid,
				HASH_REMOVE,
				&found);
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
	char				 *dsm_name;
	void				 *dsm_seg;
	TpDsmSegmentHeader	 *dsm_header;
	LocalStateCacheEntry *entry;
	size_t				  total_size;
	bool				  found;

	/* Create DSA area name for this index */
	dsm_name = psprintf("tapir.%u.%u", MyDatabaseId, index_oid);

	/* Create DSA area in TopMemoryContext for persistence across queries */
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		dsa						 = dsa_create(LWLockNewTrancheId());
		MemoryContextSwitchTo(oldcontext);
	}
	dsa_pin(dsa); /* Pin the DSA area itself for cross-backend persistence */
	dsa_pin_mapping(dsa); /* Pin this backend's mapping */

	/* Store DSA handle in small named DSM segment for recovery */
	total_size = sizeof(TpDsmSegmentHeader);
	dsm_seg	   = GetNamedDSMSegment(dsm_name, total_size, NULL, &found);
	dsm_header = (TpDsmSegmentHeader *)dsm_seg;
	dsm_header->dsm_handle = dsa_get_handle(dsa);

	/* DSA handle stored for recovery */

	/* Allocate shared state in DSA */
	shared_dp	 = dsa_allocate(dsa, sizeof(TpSharedIndexState));
	shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);

	/* Initialize shared state */
	shared_state->index_oid	 = index_oid;
	shared_state->heap_oid	 = heap_oid;
	shared_state->total_docs = 0;
	shared_state->total_len	 = 0;
	shared_state->idf_sum	 = 0.0;

	/* Allocate and initialize memtable */
	memtable_dp = dsa_allocate(dsa, sizeof(TpMemtable));
	memtable	= (TpMemtable *)dsa_get_address(dsa, memtable_dp);
	memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	memtable->total_terms		 = 0;
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;

	shared_state->memtable_dp = memtable_dp;

	/* Store shared state pointer in DSM header for recovery */
	dsm_header->shared_state_dp = shared_dp;

	/* Register in global registry using DSA pointer, not memory address */
	if (!tp_registry_register(index_oid, (TpSharedIndexState *)shared_dp))
	{
		/* If registration failed, try initializing the registry first */
		tp_registry_shmem_startup();
		if (!tp_registry_register(index_oid, (TpSharedIndexState *)shared_dp))
			elog(ERROR, "Failed to register index %u", index_oid);
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

	pfree(dsm_name);

	return local_state;
}

/*
 * Destroy a shared index state
 *
 * This is called during DROP INDEX to clean up the shared state
 */
void
tp_destroy_shared_index_state(TpSharedIndexState *shared_state)
{
	char *dsm_name;

	if (shared_state == NULL)
		return;

	/* Unregister from global registry */
	tp_registry_unregister(shared_state->index_oid);

	/* Clean up the DSM segment for this index */
	dsm_name = psprintf("tapir.%u.%u", MyDatabaseId, shared_state->index_oid);

	/* Note: There's no CancelNamedDSMSegment() in PostgreSQL's public API.
	 * Named DSM segments are automatically cleaned up when the database
	 * process exits. For now, we rely on this automatic cleanup.
	 * The DSA area will be freed when the last backend detaches. */

	pfree(dsm_name);
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

	/* DEBUG: Log what metapage contains during recovery */
	elog(NOTICE,
		 "DEBUG: Recovery metapage: magic=0x%08X, first_docid_page=%u",
		 metap->magic,
		 metap->first_docid_page);

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
		elog(DEBUG1,
			 "Recalculated IDF sum after recovery: %.6f",
			 local_state->shared->idf_sum);
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
		elog(NOTICE, "No docid pages to recover from");
		return;
	}

	/* Open the heap relation to fetch document text */
	heap_rel = relation_open(index_rel->rd_index->indrelid, AccessShareLock);

	current_page = metap->first_docid_page;

	/* DEBUG: Log metapage docid chain starting point */
	elog(NOTICE,
		 "DEBUG: Recovery starting from metapage first_docid_page=%u",
		 current_page);

	/* Scan through all docid pages */
	while (current_page != InvalidBlockNumber)
	{
		/* Read the docid page */
		docid_buf = ReadBuffer(index_rel, current_page);
		LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
		docid_page	 = BufferGetPage(docid_buf);
		docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);

		/* DEBUG: Log what we're reading from this page */
		elog(NOTICE,
			 "DEBUG: Recovery reading docid page %u, magic=0x%08X, "
			 "num_docids=%d",
			 current_page,
			 docid_header->magic,
			 docid_header->num_docids);

		/* Get docids array with proper alignment */
		docids = (ItemPointer)((char *)docid_header +
							   MAXALIGN(sizeof(TpDocidPageHeader)));

		/* Process each docid on this page */
		for (unsigned int i = 0; i < docid_header->num_docids; i++)
		{
			ItemPointer	  ctid = &docids[i];
			HeapTupleData tuple_data;

			/* DEBUG: Log each CTID being processed */
			elog(NOTICE,
				 "DEBUG: Recovery processing docid[%u] = (%u,%u)",
				 i,
				 BlockIdGetBlockNumber(&ctid->ip_blkid),
				 ctid->ip_posid);
			HeapTuple tuple = &tuple_data;
			Buffer	  heap_buf;
			bool	  valid;
			Datum	  text_datum;
			bool	  isnull;
			text	 *document_text;
			int32	  doc_length;

			/* Note: ItemPointer validation will happen in heap_fetch */

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

			/* Extract text from the first indexed column */
			text_datum = heap_getattr(
					tuple, 1, RelationGetDescr(heap_rel), &isnull);

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
