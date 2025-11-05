/*-------------------------------------------------------------------------
 *
 * registry.c
 *	  Tapir global registry implementation
 *
 * This module manages a global registry that maps index OIDs to their
 * TpSharedIndexState structures. The registry itself is stored in regular
 * PostgreSQL shared memory and is accessible to all backends.
 *
 * IDENTIFICATION
 *	  src/registry.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <miscadmin.h>
#include <storage/ipc.h>
#include <storage/lwlock.h>
#include <storage/shmem.h>
#include <utils/dsa.h>
#include <utils/memutils.h>

#include "registry.h"
#include "memory.h"

/* Backend-local pointer to the registry in shared memory */
static TpGlobalRegistry *tapir_registry = NULL;

/* Backend-local DSA area pointer */
static dsa_area *tapir_dsa = NULL;

/*
 * Request shared memory for the registry
 * Only effective when loaded via shared_preload_libraries
 * Without preloading, registry initializes lazily on first use
 */
void
tp_registry_init(void)
{
	/* Request shared memory for the registry */
	RequestAddinShmemSpace(sizeof(TpGlobalRegistry));
}

/*
 * Create or attach to the registry in shared memory
 * This is called during backend startup
 */
void
tp_registry_shmem_startup(void)
{
	bool found;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	tapir_registry = ShmemInitStruct(
			"Tapir Index Registry", sizeof(TpGlobalRegistry), &found);

	if (!found)
	{
		/* First time initialization */
		memset(tapir_registry, 0, sizeof(TpGlobalRegistry));

		/* Initialize the registry lock */
		LWLockInitialize(&tapir_registry->lock, LWLockNewTrancheId());

		/* Initialize DSA handle as invalid */
		tapir_registry->dsa_handle = DSA_HANDLE_INVALID;

		/* Initialize all entries as not in use */
		for (int i = 0; i < TP_MAX_INDEXES; i++)
		{
			tapir_registry->entries[i].index_oid	= InvalidOid;
			tapir_registry->entries[i].shared_state = NULL;
		}

		tapir_registry->num_entries = 0;
	}

	LWLockRelease(AddinShmemInitLock);

	/* Register the lock tranche */
	LWLockRegisterTranche(tapir_registry->lock.tranche, "tapir_registry");
}

/*
 * Get or create the shared DSA area
 *
 * This function is called by any backend that needs access to the DSA.
 * The first backend creates it, others attach to it.
 */
dsa_area *
tp_registry_get_dsa(void)
{
	/* Quick check if already attached in this backend */
	if (tapir_dsa != NULL)
		return tapir_dsa;

	/* Ensure registry is initialized */
	if (!tapir_registry)
	{
		tp_registry_shmem_startup();
		if (!tapir_registry)
			elog(ERROR, "Failed to initialize Tapir registry");
	}

	/* Check if DSA exists, create if needed */
	LWLockAcquire(&tapir_registry->lock, LW_EXCLUSIVE);

	if (tapir_registry->dsa_handle == DSA_HANDLE_INVALID)
	{
		/* First backend - create the DSA */
		MemoryContext oldcontext;
		int			  tranche_id = LWLockNewTrancheId();
		size_t		  init_segment_size;
		size_t		  max_segment_size;

		/*
		 * Calculate appropriate DSA segment sizes based on configured memory
		 * limit. Without this, DSA pre-allocates 46MB regardless of our
		 * actual needs, wasting shared memory.
		 *
		 * Use smaller initial segment and cap maximum segment size to avoid
		 * wasting shared memory.
		 */
		max_segment_size = tp_get_memory_limit();

		/* Start with 256KB segments instead of default 1MB */
		init_segment_size = 256 * 1024L;

		/* Make sure init size doesn't exceed max */
		if (init_segment_size > max_segment_size)
			init_segment_size = max_segment_size;

		elog(DEBUG1,
			 "Creating DSA with init_segment=%zu max_segment=%zu "
			 "(limit=%zuMB)",
			 init_segment_size,
			 max_segment_size,
			 max_segment_size / (1024 * 1024));

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		/* Register the tranche for LWLock debugging/monitoring */
		LWLockRegisterTranche(tranche_id, "pg_textsearch DSA");

		tapir_dsa  = dsa_create_ext(
				 tranche_id, init_segment_size, max_segment_size);
		MemoryContextSwitchTo(oldcontext);

		if (tapir_dsa == NULL)
		{
			LWLockRelease(&tapir_registry->lock);
			elog(ERROR, "Failed to create DSA area");
		}

		/* Set total DSA size limit to prevent over-allocation */
		dsa_set_size_limit(tapir_dsa, max_segment_size);

		elog(DEBUG1,
			 "DSA created and limited to %zuMB total",
			 max_segment_size / (1024 * 1024));

		/* Pin the DSA to keep it alive across backends */
		dsa_pin(tapir_dsa);

		/* Pin the mapping for this backend */
		dsa_pin_mapping(tapir_dsa);

		/* Store handle for other backends */
		tapir_registry->dsa_handle = dsa_get_handle(tapir_dsa);
	}
	else
	{
		/* DSA exists - attach to it */
		MemoryContext oldcontext;

		/* Register the tranche for this backend */
		/* Note: We don't know the exact tranche_id here, but DSA handles this
		 * internally */

		/* Attach in TopMemoryContext so the dsa_area structure
		 * doesn't get freed when query memory contexts are cleaned up.
		 * This prevents heap-use-after-free errors when accessing the DSA
		 * in subsequent queries. */
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		tapir_dsa  = dsa_attach(tapir_registry->dsa_handle);
		MemoryContextSwitchTo(oldcontext);

		if (tapir_dsa == NULL)
		{
			LWLockRelease(&tapir_registry->lock);
			elog(ERROR, "Failed to attach to Tapir shared DSA");
		}

		/* Pin the mapping for this backend */
		dsa_pin_mapping(tapir_dsa);
	}

	LWLockRelease(&tapir_registry->lock);

	return tapir_dsa;
}

/*
 * Register an index in the global registry
 * Returns true on success, false if registry is full
 */
bool
tp_registry_register(
		Oid index_oid, TpSharedIndexState *shared_state, dsa_pointer shared_dp)
{
	if (!tapir_registry)
	{
		/* Registry not attached in this backend - initialize it */
		tp_registry_shmem_startup();
		if (!tapir_registry)
			elog(ERROR,
				 "Failed to initialize Tapir registry for index %u",
				 index_oid);
	}

	LWLockAcquire(&tapir_registry->lock, LW_EXCLUSIVE);

	/* Check if already registered (shouldn't happen) */
	for (int i = 0; i < TP_MAX_INDEXES; i++)
	{
		if (tapir_registry->entries[i].index_oid == index_oid)
		{
			/* Update the shared state and return */
			tapir_registry->entries[i].shared_state	   = shared_state;
			tapir_registry->entries[i].shared_state_dp = shared_dp;
			LWLockRelease(&tapir_registry->lock);
			return true;
		}
	}

	/* Find an empty slot */
	for (int i = 0; i < TP_MAX_INDEXES; i++)
	{
		if (tapir_registry->entries[i].index_oid == InvalidOid)
		{
			tapir_registry->entries[i].index_oid	   = index_oid;
			tapir_registry->entries[i].shared_state	   = shared_state;
			tapir_registry->entries[i].shared_state_dp = shared_dp;
			tapir_registry->num_entries++;
			LWLockRelease(&tapir_registry->lock);
			return true;
		}
	}

	/* Registry is full - this is an error condition */
	LWLockRelease(&tapir_registry->lock);
	elog(ERROR, "Tapir registry full, cannot register index %u", index_oid);
	/* Not reached, but keeps compiler happy */
	return false;
}

/*
 * Look up an index in the registry
 * Returns the shared state pointer or NULL if not found
 */
TpSharedIndexState *
tp_registry_lookup(Oid index_oid)
{
	TpSharedIndexState *result = NULL;

	if (!tapir_registry)
	{
		/* Registry not attached in this backend - initialize it */
		tp_registry_shmem_startup();
		if (!tapir_registry)
			elog(ERROR, "Failed to initialize Tapir registry for lookup");
	}

	LWLockAcquire(&tapir_registry->lock, LW_SHARED);

	for (int i = 0; i < TP_MAX_INDEXES; i++)
	{
		if (tapir_registry->entries[i].index_oid == index_oid)
		{
			/* Return the DSA pointer cast as a TpSharedIndexState pointer
			 * The caller will convert it back to a DSA pointer */
			result = (TpSharedIndexState *)(uintptr_t)tapir_registry
							 ->entries[i]
							 .shared_state_dp;
			break;
		}
	}

	LWLockRelease(&tapir_registry->lock);
	return result;
}

/*
 * Look up an index's DSA pointer in the registry
 * Returns the DSA pointer if found, InvalidDsaPointer otherwise
 */
dsa_pointer
tp_registry_lookup_dsa(Oid index_oid)
{
	dsa_pointer result = InvalidDsaPointer;

	if (!tapir_registry)
	{
		/* Registry not attached in this backend - initialize it */
		tp_registry_shmem_startup();
		if (!tapir_registry)
			elog(ERROR, "Failed to initialize Tapir registry for lookup");
	}

	LWLockAcquire(&tapir_registry->lock, LW_SHARED);

	for (int i = 0; i < TP_MAX_INDEXES; i++)
	{
		if (tapir_registry->entries[i].index_oid == index_oid)
		{
			result = tapir_registry->entries[i].shared_state_dp;
			break;
		}
	}

	LWLockRelease(&tapir_registry->lock);
	return result;
}

/*
 * Get DSA pointer to shared state from the registry
 * Returns the DSA pointer or InvalidDsaPointer if not found
 */
dsa_pointer
tp_registry_get_shared_dp(Oid index_oid)
{
	dsa_pointer result = InvalidDsaPointer;

	if (!tapir_registry)
		return InvalidDsaPointer;

	LWLockAcquire(&tapir_registry->lock, LW_SHARED);

	for (int i = 0; i < TP_MAX_INDEXES; i++)
	{
		if (tapir_registry->entries[i].index_oid == index_oid)
		{
			result = tapir_registry->entries[i].shared_state_dp;
			break;
		}
	}

	LWLockRelease(&tapir_registry->lock);
	return result;
}

/*
 * Check if an index is registered
 * Returns true if the index is in the registry, false otherwise
 */
bool
tp_registry_is_registered(Oid index_oid)
{
	bool result = false;

	if (!tapir_registry)
	{
		/* Registry not initialized - can't be registered */
		return false;
	}

	LWLockAcquire(&tapir_registry->lock, LW_SHARED);

	for (int i = 0; i < TP_MAX_INDEXES; i++)
	{
		if (tapir_registry->entries[i].index_oid == index_oid)
		{
			result = true;
			break;
		}
	}

	LWLockRelease(&tapir_registry->lock);
	return result;
}

/*
 * Unregister an index from the registry
 * Called when an index is dropped
 */
void
tp_registry_unregister(Oid index_oid)
{
	if (!tapir_registry)
	{
		/* Registry not initialized - nothing to unregister */
		return;
	}

	LWLockAcquire(&tapir_registry->lock, LW_EXCLUSIVE);

	for (int i = 0; i < TP_MAX_INDEXES; i++)
	{
		if (tapir_registry->entries[i].index_oid == index_oid)
		{
			tapir_registry->entries[i].index_oid	   = InvalidOid;
			tapir_registry->entries[i].shared_state	   = NULL;
			tapir_registry->entries[i].shared_state_dp = InvalidDsaPointer;
			tapir_registry->num_entries--;
			break;
		}
	}

	LWLockRelease(&tapir_registry->lock);
}

/*
 * Reset the DSA handle in the registry
 *
 * This is called when the extension is dropped to clear all index entries.
 * However, we DO NOT invalidate the DSA handle itself, as other backends
 * may still have references to it.
 */
void
tp_registry_reset_dsa(void)
{
	if (!tapir_registry)
		return;

	LWLockAcquire(&tapir_registry->lock, LW_EXCLUSIVE);

	/* Clear all index entries */
	for (int i = 0; i < TP_MAX_INDEXES; i++)
	{
		tapir_registry->entries[i].index_oid	   = InvalidOid;
		tapir_registry->entries[i].shared_state	   = NULL;
		tapir_registry->entries[i].shared_state_dp = InvalidDsaPointer;
	}
	tapir_registry->num_entries = 0;

	LWLockRelease(&tapir_registry->lock);
}
