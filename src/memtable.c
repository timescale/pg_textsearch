/*-------------------------------------------------------------------------
 *
 * memtable.c
 *	  Tapir BM25 in-memory table implementation
 *
 * IDENTIFICATION
 *	  src/memtable.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/heapam.h>
#include <catalog/namespace.h>
#include <catalog/pg_database.h>
#include <commands/dbcommands.h>
#include <miscadmin.h>
#include <storage/dsm_registry.h>
#include <storage/lwlock.h>
#include <utils/builtins.h>
#include <utils/dsa.h>
#include <utils/guc.h>
#include <utils/hsearch.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/syscache.h>

#include "common/hashfn.h"
#include "constants.h"
#include "index.h"
#include "memtable.h"
#include "posting.h"
#include "stringtable.h"

/* Backend-local index state cache to prevent multiple DSM segment attachments
 */
static HTAB *index_state_cache = NULL;

/* IndexStateCacheEntry type definition moved to memtable.h */

/* Per-backend hash table for query limits */
HTAB *tp_query_limits_hash = NULL;

/* GUC variables */
int tp_index_memory_limit = TP_DEFAULT_INDEX_MEMORY_LIMIT;
int tp_default_limit	  = TP_DEFAULT_QUERY_LIMIT;

/* Transaction-level lock tracking removed - handled by DSA LWLocks */

/*
 * Generate human-readable DSM segment name for an index
 * Format: "tapir.dbname.schema.indexname"
 */
char *
tp_get_dsm_segment_name(Oid index_oid)
{
	/*
	 * Build segment name: tapir.dbid.oid for uniqueness Including database
	 * OID ensures no cross-database conflicts Using index OID ensures each
	 * index gets its own segment even if names are reused
	 */
	return psprintf("tapir.%u.%u", MyDatabaseId, index_oid);
}

/*
 * Initialize the backend-local index state cache
 */
static void
init_index_state_cache(void)
{
	HASHCTL ctl;

	if (index_state_cache != NULL)
		return;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize	  = sizeof(Oid);
	ctl.entrysize = sizeof(IndexStateCacheEntry);
	ctl.hcxt	  = TopMemoryContext;

	index_state_cache = hash_create(
			"Tapir Index State Cache",
			16, /* initial size */
			&ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Get cached index state and DSA area, preventing multiple DSM attachments
 */
IndexStateCacheEntry *
get_cached_index_state(Oid index_oid)
{
	IndexStateCacheEntry *entry;

	/* Initialize cache if needed */
	init_index_state_cache();

	/* Check if we already have this index cached */
	entry = (IndexStateCacheEntry *)
			hash_search(index_state_cache, &index_oid, HASH_FIND, NULL);

	if (entry != NULL)
	{
		elog(DEBUG2, "Found cached index state for index %u", index_oid);
		return entry;
	}

	elog(DEBUG2, "No cached index state for index %u", index_oid);
	return NULL;
}

/*
 * Cache an index state and DSA area to prevent multiple DSM attachments
 */
static void
cache_index_state(Oid index_oid, TpIndexState *state, dsa_area *area)
{
	IndexStateCacheEntry *entry;
	bool				  found;

	/* Initialize cache if needed */
	init_index_state_cache();

	/* Cache the state and area */
	entry = (IndexStateCacheEntry *)
			hash_search(index_state_cache, &index_oid, HASH_ENTER, &found);
	entry->state = state;
	entry->area	 = area;

	elog(DEBUG2,
		 "Cached index state for index %u (found=%d), area=%p",
		 index_oid,
		 found,
		 area);
}

/*
 * Initialize DSA callback for new index
 * Called once when the DSA segment is first created
 */
static void
tp_init_index_dsa_callback(void *ptr)
{
	TpIndexState *state = (TpIndexState *)ptr;

	/* Use fixed tranche IDs that are consistent across all backends */
	state->string_tranche_id	  = TAPIR_TRANCHE_STRING;
	state->posting_tranche_id	  = TAPIR_TRANCHE_POSTING;
	state->corpus_tranche_id	  = TAPIR_TRANCHE_CORPUS;
	state->doc_lengths_tranche_id = TAPIR_TRANCHE_DOC_LENGTHS;

	/* Initialize locks with the fixed tranche IDs */
	LWLockInitialize(&state->string_interning_lock, state->string_tranche_id);
	LWLockInitialize(&state->posting_lists_lock, state->posting_tranche_id);
	LWLockInitialize(&state->corpus_stats_lock, state->corpus_tranche_id);
	LWLockInitialize(&state->doc_lengths_lock, state->doc_lengths_tranche_id);

	/* Initialize DSA area handle - will be set by caller after callback */
	state->area_handle	   = DSA_HANDLE_INVALID;
	state->dsa_initialized = false;

	/* Initialize state */
	state->string_hash_handle = DSHASH_HANDLE_INVALID;
	state->total_terms		  = 0;
	state->doc_lengths_hash	  = NULL; /* Will be attached when needed */

	/* Initialize corpus statistics */
	memset(&state->stats, 0, sizeof(TpCorpusStatistics));
	state->stats.k1				   = 1.2f;
	state->stats.b				   = 0.75f;
	state->stats.last_checkpoint   = GetCurrentTimestamp();
	state->stats.segment_threshold = TP_DEFAULT_SEGMENT_THRESHOLD;

	elog(DEBUG1,
		 "Initialized DSA for index with lock tranches: string=%d, "
		 "posting=%d, corpus=%d, "
		 "doc_lengths=%d",
		 state->string_tranche_id,
		 state->posting_tranche_id,
		 state->corpus_tranche_id,
		 state->doc_lengths_tranche_id);
}

/*
 * Get or create DSA area for an index
 *
 * Simple approach:
 * 1. Name: "tapir.{database_id}.{index_oid}"
 * 2. GetNamedDSMSegment() gets or creates the segment
 * 3. If new segment (!found): create DSA area
 * 4. If existing segment (found): attach to DSA area
 * 5. Always cache to avoid multiple attachments per backend
 */
TpIndexState *
tp_get_or_create_index_dsa(Oid index_oid, dsa_area **area_out)
{
	char				 *segment_name;
	bool				  found;
	TpIndexState		 *state;
	dsa_area			 *area = NULL;
	IndexStateCacheEntry *cache_entry;
	MemoryContext		  oldcontext;

	/* Step 1: Check backend-local cache */
	cache_entry = get_cached_index_state(index_oid);
	if (cache_entry != NULL)
	{
		elog(DEBUG2, "Using cached index state for index %u", index_oid);
		if (area_out)
			*area_out = cache_entry->area;
		return cache_entry->state;
	}

	/* Step 2: Get or create the named DSM segment */
	segment_name = tp_get_dsm_segment_name(index_oid);

	state = GetNamedDSMSegment(
			segment_name,
			sizeof(TpIndexState),
			tp_init_index_dsa_callback,
			&found);

	/* Store the index OID */
	state->index_oid = index_oid;

	/* Register lock tranches in this backend */
	LWLockRegisterTranche(state->string_tranche_id, "tapir_string");
	LWLockRegisterTranche(state->posting_tranche_id, "tapir_posting");
	LWLockRegisterTranche(state->corpus_tranche_id, "tapir_corpus");
	LWLockRegisterTranche(state->doc_lengths_tranche_id, "tapir_doc_lengths");

	/* Step 3: Handle DSA area based on whether segment is new or existing */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	if (!found)
	{
		/* New segment - create DSA area */
		area = dsa_create(TAPIR_TRANCHE_POSTING);
		if (!area)
			elog(ERROR, "Failed to create DSA area for index %u", index_oid);

		state->area_handle	   = dsa_get_handle(area);
		state->dsa_initialized = true;

		/* Pin for persistence */
		dsa_pin(area);
		dsa_pin_mapping(area);
	}
	else
	{
		/* Existing segment - attach to DSA area */

		if (state->area_handle == DSA_HANDLE_INVALID)
		{
			/* Shouldn't happen but handle gracefully */
			elog(WARNING,
				 "Existing segment has invalid DSA handle, creating new area");
			area = dsa_create(TAPIR_TRANCHE_POSTING);
			if (!area)
				elog(ERROR,
					 "Failed to create DSA area for index %u",
					 index_oid);

			state->area_handle	   = dsa_get_handle(area);
			state->dsa_initialized = true;
			dsa_pin(area);
			dsa_pin_mapping(area);
		}
		else
		{
			area = dsa_attach(state->area_handle);
			if (!area)
				elog(ERROR,
					 "Failed to attach to DSA area %u for index %u",
					 state->area_handle,
					 index_oid);

			/* Pin mapping in this backend */
			dsa_pin_mapping(area);
		}
	}

	MemoryContextSwitchTo(oldcontext);

	/* Step 4: Cache the state and area */
	cache_index_state(index_oid, state, area);

	/* Step 5: Return results */
	if (area_out)
		*area_out = area;

	return state;
}

/*
 * Destroy DSA area for an index (called when index is dropped)
 */
void
tp_destroy_index_dsa(Oid index_oid)
{
	char				 *segment_name;
	IndexStateCacheEntry *cache_entry;
	bool				  found;

	segment_name = tp_get_dsm_segment_name(index_oid);

	/* First, remove from cache if present */
	if (index_state_cache != NULL)
	{
		cache_entry = (IndexStateCacheEntry *)hash_search(
				index_state_cache, &index_oid, HASH_REMOVE, &found);
		if (found && cache_entry)
		{
			elog(DEBUG2, "Removed index %u from cache", index_oid);

			/*
			 * Note: We don't detach from the DSA area here because: 1. The
			 * area might already be detached/freed by PostgreSQL 2. Other
			 * backends might still be using it 3. It will be cleaned up when
			 * the DSM segment is destroyed
			 *
			 * Just clear our local reference.
			 */
			cache_entry->area  = NULL;
			cache_entry->state = NULL;
		}
	}

	/*
	 * Log that we're cleaning up the index. We don't need to check if the DSM
	 * segment exists or destroy it here because: 1. Other backends might
	 * still be using it 2. PostgreSQL will clean it up when all backends
	 * detach 3. The segment will be reused if an index with the same OID is
	 * created
	 */
	elog(DEBUG1, "Cleaned up DSA references for dropped index %u", index_oid);

	pfree(segment_name);
}

/*
 * Get DSA area for an index
 * Simple helper that calls tp_get_or_create_index_dsa to get the area
 */
dsa_area *
tp_get_dsa_area_for_index(TpIndexState *index_state, Oid index_oid)
{
	dsa_area *area = NULL;

	/* Use the index OID from the state if provided */
	Oid oid = index_state->index_oid != InvalidOid ? index_state->index_oid
												   : index_oid;

	/* Get the state and area (will use cache if available) */
	tp_get_or_create_index_dsa(oid, &area);

	if (!area)
		elog(ERROR, "Failed to get DSA area for index %u", oid);

	return area;
}

/*
 * DSA memory allocation wrappers
 */
dsa_pointer
tp_dsa_allocate(dsa_area *area, Size size)
{
	return dsa_allocate(area, size);
}

void
tp_dsa_free(dsa_area *area, dsa_pointer dp)
{
	dsa_free(area, dp);
}

void *
tp_dsa_get_address(dsa_area *area, dsa_pointer dp)
{
	return dsa_get_address(area, dp);
}

/*
 * Clean up shared memory for a dropped index
 */
void
tp_cleanup_index_shared_memory(Oid index_oid)
{
	/* This will be called from the object access hook in mod.c */
	elog(DEBUG1,
		 "tp_cleanup_index_shared_memory called with OID %u",
		 index_oid);

	/* Validate OID - skip invalid OIDs */
	if (!OidIsValid(index_oid))
	{
		elog(DEBUG2, "Skipping cleanup for invalid OID");
		return;
	}

	/* Only clean up if we have a cached entry for this index */
	if (index_state_cache != NULL)
	{
		bool found;

		(void)hash_search(index_state_cache, &index_oid, HASH_FIND, &found);
		if (found)
		{
			elog(DEBUG1, "Cleaning up Tapir index %u", index_oid);
			tp_destroy_index_dsa(index_oid);
		}
		else
		{
			elog(DEBUG2,
				 "Skipping cleanup for non-Tapir relation %u",
				 index_oid);
		}
	}
	else
	{
		elog(DEBUG2,
			 "Index state cache not initialized, skipping cleanup for OID %u",
			 index_oid);
	}
}
