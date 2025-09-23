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
#include <storage/bufmgr.h>
#include <utils/dsa.h>
#include <utils/memutils.h>
#include <utils/hsearch.h>
#include <storage/dsm_registry.h>
#include <storage/dsm.h>
#include <miscadmin.h>
#include <lib/dshash.h>

#include "state.h"
#include "registry.h"
#include "metapage.h"

/* Cache of local index states */
static HTAB *local_state_cache = NULL;

typedef struct LocalStateCacheEntry
{
	Oid index_oid;					/* Hash key */
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
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(LocalStateCacheEntry);
	ctl.hcxt = TopMemoryContext;

	local_state_cache = hash_create("Tapir Local State Cache",
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
	TpLocalIndexState *local_state;
	TpSharedIndexState *shared_state;
	bool found;
	char *dsm_name;
	void *dsm_seg;
	void *dsa_space;
	size_t total_size;
	dsa_area *dsa;

	/* Initialize cache if needed */
	init_local_state_cache();

	/* Debug logging removed for clean test output */

	/* Check cache first */
	entry = (LocalStateCacheEntry *)
		hash_search(local_state_cache, &index_oid, HASH_FIND, NULL);
	if (entry != NULL)
		return entry->local_state;

	/* Look up shared state in registry */
	shared_state = tp_registry_lookup(index_oid);
	/* Registry lookup debug logging removed */
	if (shared_state == NULL)
	{
		/* Starting recovery path */
		/* No registry entry - check if DSM segment exists for crash recovery */
		dsm_name = psprintf("tapir.%u.%u", MyDatabaseId, index_oid);
		total_size = sizeof(TpDsmSegmentHeader);
		dsm_seg = GetNamedDSMSegment(dsm_name, total_size, NULL, &found);
		/* DSM segment lookup completed */

		if (found)
		{
			/* DSM segment exists - check if it has valid contents */
			TpDsmSegmentHeader *dsm_header = (TpDsmSegmentHeader *) dsm_seg;

			elog(NOTICE, "Recovery: DSM header check - handle: %u, shared_state_dp: %lu",
				 dsm_header->dsm_handle, (unsigned long)dsm_header->shared_state_dp);

			if (dsm_header->dsm_handle != DSM_HANDLE_INVALID &&
				DsaPointerIsValid(dsm_header->shared_state_dp))
			{
				/* DSM segment has valid contents - try to attach */
				elog(NOTICE, "Recovery: attempting to attach to DSA handle %u for index %u",
					 dsm_header->dsm_handle, index_oid);
				dsa = dsa_attach(dsm_header->dsm_handle);
				if (dsa == NULL)
				{
					elog(WARNING, "Recovery: dsa_attach failed, rebuilding from disk");
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
						elog(WARNING, "Failed to register recovered index %u", index_oid);
						dsa_detach(dsa);
						pfree(dsm_name);
						return NULL;
					}

					elog(NOTICE, "Recovered index state for index %u", index_oid);
					/* Continue with normal local state creation below */
				}
			}
		}

		/* If we get here, either no DSM segment found or DSM contents invalid - rebuild from disk */
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

	/* If we reach here with shared_state set, continue with normal local state creation */
	if (shared_state != NULL)
	{
		/* Allocate local state */
		local_state = (TpLocalIndexState *)
			MemoryContextAlloc(TopMemoryContext, sizeof(TpLocalIndexState));
		local_state->shared = shared_state;
		local_state->dsa = dsa;

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
		hash_search(local_state_cache, &local_state->shared->index_oid,
					HASH_REMOVE, &found);
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
	TpSharedIndexState *shared_state;
	TpLocalIndexState *local_state;
	TpMemtable *memtable;
	dsa_area *dsa;
	dsa_pointer shared_dp;
	dsa_pointer memtable_dp;
	char *dsm_name;
	void *dsm_seg;
	TpDsmSegmentHeader *dsm_header;
	LocalStateCacheEntry *entry;
	dsm_segment *dsm_segment;
	void *dsa_space;
	size_t total_size;
	bool found;

	/* Create DSA area name for this index */
	dsm_name = psprintf("tapir.%u.%u", MyDatabaseId, index_oid);

	/* Use proven working DSA creation approach */
	dsa = dsa_create(LWLockNewTrancheId());
	dsa_pin(dsa);         /* Pin the DSA area itself for cross-backend persistence */
	dsa_pin_mapping(dsa); /* Pin this backend's mapping */

	/* Store DSA handle in small named DSM segment for recovery */
	total_size = sizeof(TpDsmSegmentHeader);
	dsm_seg = GetNamedDSMSegment(dsm_name, total_size, NULL, &found);
	dsm_header = (TpDsmSegmentHeader *) dsm_seg;
	dsm_header->dsm_handle = dsa_get_handle(dsa);

	/* DSA handle stored for recovery */

	/* Allocate shared state in DSA */
	shared_dp = dsa_allocate(dsa, sizeof(TpSharedIndexState));
	shared_state = (TpSharedIndexState *) dsa_get_address(dsa, shared_dp);

	/* Initialize shared state */
	shared_state->index_oid = index_oid;
	shared_state->heap_oid = heap_oid;
	shared_state->total_docs = 0;
	shared_state->total_len = 0;

	/* Allocate and initialize memtable */
	memtable_dp = dsa_allocate(dsa, sizeof(TpMemtable));
	memtable = (TpMemtable *) dsa_get_address(dsa, memtable_dp);
	memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	memtable->total_terms = 0;
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;

	shared_state->memtable_dp = memtable_dp;

	/* Store shared state pointer in DSM header for recovery */
	dsm_header->shared_state_dp = shared_dp;

	/* Register in global registry - initialize registry if needed */
	if (!tp_registry_register(index_oid, shared_state))
	{
		/* If registration failed, try initializing the registry first */
		tp_registry_shmem_startup();
		if (!tp_registry_register(index_oid, shared_state))
			elog(ERROR, "Failed to register index %u", index_oid);
	}

	/* Create local state for the creating backend */
	local_state = (TpLocalIndexState *)
		MemoryContextAlloc(TopMemoryContext, sizeof(TpLocalIndexState));
	local_state->shared = shared_state;
	local_state->dsa = dsa;

	/* Cache the local state */
	init_local_state_cache();
	entry = (LocalStateCacheEntry *)
		hash_search(local_state_cache, &index_oid, HASH_ENTER, &found);
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
	Relation index_rel;
	TpIndexMetaPage metap;
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

	/* Create fresh state first */
	/* Creating fresh state for restart recovery */
	local_state = tp_create_shared_index_state(index_oid, index_rel->rd_index->indrelid);

	if (local_state != NULL)
	{
		/* Recovery would go here, but for now just return the fresh state */
		/* Using fresh state (docid recovery not yet implemented) */
		/* TODO: Implement proper docid page recovery after fixing recursion */
	}

	/* Clean up */
	if (metap)
		pfree(metap);
	index_close(index_rel, AccessShareLock);

	return local_state;
}

