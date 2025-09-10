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

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "constants.h"
#include "stringtable.h"
#include "index.h"
#include "memtable.h"
#include "posting.h"

/* Backend-local index state cache to prevent multiple DSM segment attachments */
static HTAB *index_state_cache = NULL;

/* IndexStateCacheEntry type definition moved to memtable.h */

/* Per-backend hash table for query limits */
HTAB	   *tp_query_limits_hash = NULL;

/* GUC variables */
int			tp_index_memory_limit = TP_DEFAULT_INDEX_MEMORY_LIMIT;
int			tp_default_limit = TP_DEFAULT_QUERY_LIMIT;

/* Transaction-level lock tracking (per-backend) */
bool tp_transaction_lock_held = false;

/*
 * Generate human-readable DSM segment name for an index
 * Format: "tapir.dbname.schema.indexname"
 */
char *
tp_get_dsm_segment_name(Oid index_oid)
{
	/* Build segment name: tapir.dbid.oid for uniqueness
	 * Including database OID ensures no cross-database conflicts
	 * Using index OID ensures each index gets its own segment even if names are reused */
	return psprintf("tapir.%u.%u", MyDatabaseId, index_oid);
}

/*
 * Initialize the backend-local index state cache
 */
static void
init_index_state_cache(void)
{
	HASHCTL		ctl;

	if (index_state_cache != NULL)
		return;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(IndexStateCacheEntry);
	ctl.hcxt = TopMemoryContext;

	index_state_cache = hash_create("Tapir Index State Cache",
								 16,		/* initial size */
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
	entry = (IndexStateCacheEntry *) hash_search(index_state_cache,
												  &index_oid,
												  HASH_FIND,
												  NULL);
	
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
	bool		found;

	/* Initialize cache if needed */
	init_index_state_cache();

	/* Cache the state and area */
	entry = (IndexStateCacheEntry *) hash_search(index_state_cache,
												  &index_oid,
												  HASH_ENTER,
												  &found);
	entry->state = state;
	entry->area = area;
	
	elog(DEBUG2, "Cached index state for index %u (found=%d), area=%p", index_oid, found, area);
}

/*
 * Initialize DSA callback for new index
 * Called once when the DSA segment is first created
 */
static void
tp_init_index_dsa_callback(void *ptr)
{
	TpIndexState *state = (TpIndexState *) ptr;
	
	/* Use fixed tranche IDs that are consistent across all backends */
	state->string_tranche_id = TAPIR_TRANCHE_STRING;
	state->posting_tranche_id = TAPIR_TRANCHE_POSTING;
	state->corpus_tranche_id = TAPIR_TRANCHE_CORPUS;
	
	/* Initialize locks with the fixed tranche IDs */
	LWLockInitialize(&state->string_interning_lock, state->string_tranche_id);
	LWLockInitialize(&state->posting_lists_lock, state->posting_tranche_id);
	LWLockInitialize(&state->corpus_stats_lock, state->corpus_tranche_id);
	
	/* Initialize DSA area handle - will be set by caller after callback */
	state->area_handle = DSA_HANDLE_INVALID;
	state->dsa_initialized = false;
	
	/* Initialize state */
	state->string_hash_dp = InvalidDsaPointer;
	state->total_terms = 0;
	
	/* Initialize corpus statistics */
	memset(&state->stats, 0, sizeof(TpCorpusStatistics));
	state->stats.k1 = 1.2f;
	state->stats.b = 0.75f;
	state->stats.last_checkpoint = GetCurrentTimestamp();
	state->stats.segment_threshold = TP_DEFAULT_SEGMENT_THRESHOLD;
	
	elog(DEBUG1, "Initialized DSA for index with lock tranches: string=%d, posting=%d, corpus=%d",
		 state->string_tranche_id, state->posting_tranche_id, state->corpus_tranche_id);
}

/*
 * Get or create DSA area for an index
 * Returns the TpIndexState and optionally the DSA area
 * This is the unified function that replaces tp_create_index_dsa and tp_attach_index_dsa
 */
TpIndexState *
tp_get_or_create_index_dsa(Oid index_oid, dsa_area **area_out)
{
	char	   *segment_name;
	bool		found;
	TpIndexState *state;
	dsa_area   *area = NULL;
	IndexStateCacheEntry *cache_entry;
	
	/* Check if we already have this index cached to prevent multiple attachments */
	cache_entry = get_cached_index_state(index_oid);
	if (cache_entry != NULL)
	{
		elog(DEBUG2, "Using cached index state for index %u, area=%p", index_oid, cache_entry->area);
		if (area_out)
		{
			*area_out = cache_entry->area;
		}
		return cache_entry->state;
	}
	
	elog(DEBUG2, "No cache found, creating/attaching DSA for index %u", index_oid);
	
	segment_name = tp_get_dsm_segment_name(index_oid);
	
	elog(DEBUG1, "Getting DSM segment: name='%s' for index_oid=%u", segment_name, index_oid);
	
	state = GetNamedDSMSegment(segment_name,
							   sizeof(TpIndexState),
							   tp_init_index_dsa_callback,
							   &found);
	
	elog(DEBUG1, "Got DSM segment: state=%p, found=%d, existing_oid=%u, requested_oid=%u",
		 state, found, state->index_oid, index_oid);
	
	/* Store the index OID in the state */
	state->index_oid = index_oid;
	
	/* Register lock tranches in this backend */
	LWLockRegisterTranche(state->string_tranche_id, "tapir_string");
	LWLockRegisterTranche(state->posting_tranche_id, "tapir_posting");
	LWLockRegisterTranche(state->corpus_tranche_id, "tapir_corpus");
	
	if (!found)
	{
		elog(DEBUG1, "Created new DSA for index %u", index_oid);
	}
	else
	{
		elog(DEBUG2, "Attached to existing DSA for index %u", index_oid);
		/* Don't reset stats here - it causes problems with concurrent access.
		 * Stats will be reset in tp_build when needed. */
	}
	
	/* Create or attach to DSA area */
	elog(DEBUG2, "DSA area logic: found=%d, area_handle=%u", found, state->area_handle);
	if (!found || state->area_handle == DSA_HANDLE_INVALID)
	{
		MemoryContext oldcontext;
		
		/* New segment or existing segment without DSA area - create the DSA area */
		elog(DEBUG1, "Creating DSA area for index %u", index_oid);
		
		/* 
		 * Create DSA area with proper pinning for persistence.
		 * The area will automatically pin its underlying DSM segments.
		 * Use the fixed posting tranche ID for consistency across backends.
		 * Switch to TopMemoryContext to ensure DSA control structures persist.
		 */
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		area = dsa_create(TAPIR_TRANCHE_POSTING);
		MemoryContextSwitchTo(oldcontext);
		if (area)
		{
			state->area_handle = dsa_get_handle(area);
			state->dsa_initialized = true;
			elog(DEBUG1, "Backend created DSA area: index_oid=%u, area_handle=%u, area=%p", 
				 index_oid, state->area_handle, area);
			
			/* 
			 * Pin the DSA area to make it persistent across backend processes.
			 * This ensures the area survives even if all backends detach.
			 */
			PG_TRY();
			{
				dsa_pin(area);
				elog(DEBUG2, "Successfully pinned DSA area for index %u", index_oid);
			}
			PG_CATCH();
			{
				/* DSA area might already be pinned - that's okay */
				FlushErrorState();
				elog(DEBUG2, "DSA area for index %u already pinned", index_oid);
			}
			PG_END_TRY();
			
			/* 
			 * Pin mapping to prevent cleanup in this backend.
			 * This is critical for ensuring the DSA area remains accessible.
			 */
			dsa_pin_mapping(area);
			
			if (!found)
				elog(DEBUG1, "Created and pinned DSA area with handle %u for new index %u", 
					 state->area_handle, index_oid);
			else
				elog(DEBUG1, "Created and pinned DSA area with handle %u for existing segment index %u", 
					 state->area_handle, index_oid);
		}
		else
		{
			elog(ERROR, "Failed to create DSA area for index %u", index_oid);
		}
	}
	else
	{
		/* Existing segment with valid DSA handle - attach to existing DSA area if needed */
		if (area_out)
		{
			MemoryContext oldcontext;
			
			elog(DEBUG1, "Backend attempting to attach to DSA area: index_oid=%u, area_handle=%u, dsa_initialized=%s", 
				 index_oid, state->area_handle, state->dsa_initialized ? "true" : "false");
			
			/* Switch to TopMemoryContext to ensure DSA control structures persist */
			oldcontext = MemoryContextSwitchTo(TopMemoryContext);
			area = dsa_attach(state->area_handle);
			MemoryContextSwitchTo(oldcontext);
			
			if (area == NULL)
			{
				elog(WARNING, "Failed to attach to DSA area for index %u, handle %u - recreating", 
					 index_oid, state->area_handle);
				/* DSA area no longer valid, create a new one */
				oldcontext = MemoryContextSwitchTo(TopMemoryContext);
				area = dsa_create(TAPIR_TRANCHE_POSTING);
				MemoryContextSwitchTo(oldcontext);
				if (area)
				{
					state->area_handle = dsa_get_handle(area);
					state->dsa_initialized = true;
					
					/* Pin the recreated DSA area for persistence */
					PG_TRY();
					{
						dsa_pin(area);
						elog(DEBUG2, "Successfully pinned recreated DSA area for index %u", index_oid);
					}
					PG_CATCH();
					{
						/* DSA area might already be pinned - that's okay */
						FlushErrorState();
						elog(DEBUG2, "Recreated DSA area for index %u already pinned", index_oid);
					}
					PG_END_TRY();
					
					dsa_pin_mapping(area);
					
					elog(DEBUG1, "Recreated and pinned DSA area with handle %u for index %u", 
						 state->area_handle, index_oid);
				}
				else
				{
					elog(ERROR, "Failed to recreate DSA area for index %u", index_oid);
				}
			}
			else
			{
				/* Pin mapping to prevent automatic cleanup in this backend */
				dsa_pin_mapping(area);
				elog(DEBUG2, "Attached to existing DSA area for index %u, handle %u (pinned mapping)", 
					 index_oid, state->area_handle);
			}
		}
		else
		{
			elog(DEBUG2, "Using existing DSA state for index %u without attaching area", index_oid);
		}
	}
	
	if (area_out)
	{
		*area_out = area;
	}
	
	/* Cache the state and area - area pointer is stable within this backend session due to dsa_pin_mapping */
	elog(DEBUG1, "Caching index state: index_oid=%u, state=%p, area=%p, segment_name='%s'",
		 index_oid, state, area, segment_name);
	cache_index_state(index_oid, state, area);
	
	pfree(segment_name);
	return state;
}

/*
 * Destroy DSA area for an index (called when index is dropped)
 */
void
tp_destroy_index_dsa(Oid index_oid)
{
	char	   *segment_name;
	IndexStateCacheEntry *cache_entry;
	bool		found;
	
	segment_name = tp_get_dsm_segment_name(index_oid);
	
	/* First, remove from cache if present */
	if (index_state_cache != NULL)
	{
		cache_entry = (IndexStateCacheEntry *) hash_search(index_state_cache,
															&index_oid,
															HASH_REMOVE,
															&found);
		if (found && cache_entry)
		{
			elog(DEBUG2, "Removed index %u from cache", index_oid);
			
			/* 
			 * Note: We don't detach from the DSA area here because:
			 * 1. The area might already be detached/freed by PostgreSQL
			 * 2. Other backends might still be using it
			 * 3. It will be cleaned up when the DSM segment is destroyed
			 * 
			 * Just clear our local reference.
			 */
			cache_entry->area = NULL;
			cache_entry->state = NULL;
		}
	}
	
	/* 
	 * Log whether the DSM segment exists.
	 * We can't safely destroy it here because:
	 * 1. Other backends might still be using it
	 * 2. PostgreSQL will clean it up when all backends detach
	 * 3. The segment will be reused if an index with the same OID is created
	 */
	{
		bool found;
		TpIndexState *state = GetNamedDSMSegment(segment_name, 0, NULL, &found);
		if (state != NULL && found)
		{
			elog(DEBUG1, "DSM segment %s exists for dropped index %u", 
				 segment_name, index_oid);
		}
		else
		{
			elog(DEBUG1, "No DSM segment found for dropped index %u", index_oid);
		}
	}
	
	pfree(segment_name);
}

/*
 * Get DSA area for an index (attach using DSA handle from index state)
 */
dsa_area *
tp_get_dsa_area_for_index(TpIndexState *index_state, Oid index_oid)
{
	IndexStateCacheEntry *cache_entry;
	dsa_area *area;
	
	/* Use the index OID from the state if provided, otherwise use the parameter */
	Oid oid = index_state->index_oid != InvalidOid ? index_state->index_oid : index_oid;
	
	/* First try cache - area pointer is stable within backend session due to dsa_pin_mapping */
	cache_entry = get_cached_index_state(oid);
	if (cache_entry != NULL && cache_entry->area != NULL)
	{
		elog(DEBUG2, "Retrieved cached DSA area for index %u, area=%p", oid, cache_entry->area);
		return cache_entry->area;
	}
	
	/* Not cached or area is NULL - attach using DSA handle from index state */
	if (index_state->area_handle == DSA_HANDLE_INVALID)
	{
		elog(WARNING, "DSA handle invalid for index %u", oid);
		return NULL;
	}
	
	elog(DEBUG1, "Attaching to DSA area for index %u using handle %u", oid, index_state->area_handle);
	
	/* Switch to TopMemoryContext to ensure DSA control structures persist */
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		area = dsa_attach(index_state->area_handle);
		MemoryContextSwitchTo(oldcontext);
	}
	
	if (area == NULL)
	{
		elog(ERROR, "Failed to attach to DSA area for index %u, handle %u", oid, index_state->area_handle);
		return NULL;
	}
	elog(DEBUG1, "Successfully attached to DSA area %p for index %u", area, oid);
	
	/* Pin mapping to prevent automatic cleanup in this backend */
	dsa_pin_mapping(area);
	elog(DEBUG2, "Successfully attached to DSA area for index %u (pinned mapping)", oid);
	
	/* Cache the area for future use in this backend */
	if (cache_entry != NULL)
	{
		cache_entry->area = area;
		elog(DEBUG2, "Updated cache with DSA area for index %u", oid);
	}
	else
	{
		/* Cache both state and area */
		cache_index_state(oid, index_state, area);
		elog(DEBUG2, "Cached index state and DSA area for index %u", oid);
	}
	
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
	tp_destroy_index_dsa(index_oid);
}


/*
 * Transaction-level lock management
 * NOTE: With DSA-based per-index approach, transaction locks would need 
 * to be managed per-index. For now, these are placeholders.
 */
void
tp_transaction_lock_acquire(void)
{
	/* TODO: Implement per-index transaction locks if needed */
	tp_transaction_lock_held = true;
	elog(DEBUG2, "Tapir transaction lock acquired (placeholder)");
}

void
tp_transaction_lock_release(void)
{
	/* TODO: Implement per-index transaction locks if needed */
	tp_transaction_lock_held = false;
	elog(DEBUG2, "Tapir transaction lock released (placeholder)");
}

/*
 * String interning functions - stubs for now
 * These will be implemented per-index when the string table is converted to DSA
 */
void
tp_intern_string(const char *term)
{
	/* TODO: Implement per-index string interning */
	elog(DEBUG3, "String interning placeholder for term: %.20s", term);
}

void
tp_intern_string_len(const char *term, int term_len)
{
	/* TODO: Implement per-index string interning */
	elog(DEBUG3, "String interning placeholder for term: %.*s", term_len, term);
}
