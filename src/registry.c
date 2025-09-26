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

#include <storage/ipc.h>
#include <storage/lwlock.h>
#include <storage/shmem.h>
#include <utils/memutils.h>

#include "registry.h"

/* Backend-local pointer to the registry in shared memory */
static TpGlobalRegistry *tapir_registry = NULL;

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
 * Register an index in the global registry
 * Returns true on success, false if registry is full
 */
bool
tp_registry_register(Oid index_oid, TpSharedIndexState *shared_state)
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
			tapir_registry->entries[i].shared_state = shared_state;
			LWLockRelease(&tapir_registry->lock);
			return true;
		}
	}

	/* Find an empty slot */
	for (int i = 0; i < TP_MAX_INDEXES; i++)
	{
		if (tapir_registry->entries[i].index_oid == InvalidOid)
		{
			tapir_registry->entries[i].index_oid	= index_oid;
			tapir_registry->entries[i].shared_state = shared_state;
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
			result = tapir_registry->entries[i].shared_state;
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
			tapir_registry->entries[i].index_oid	= InvalidOid;
			tapir_registry->entries[i].shared_state = NULL;
			tapir_registry->num_entries--;
			break;
		}
	}

	LWLockRelease(&tapir_registry->lock);
}
