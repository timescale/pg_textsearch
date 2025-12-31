/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * registry.c - Global registry mapping index OIDs to shared state
 *
 * Uses a dshash (dynamic shared hash table) for O(1) lookups and no
 * limit on the number of indexes (beyond available memory).
 */
#include <postgres.h>

#include <access/hash.h>
#include <lib/dshash.h>
#include <miscadmin.h>
#include <storage/ipc.h>
#include <storage/lwlock.h>
#include <storage/shmem.h>
#include <utils/dsa.h>
#include <utils/memutils.h>

#include "state/registry.h"

/* Backend-local pointer to the registry in shared memory */
static TpGlobalRegistry *tapir_registry = NULL;

/* Backend-local DSA area pointer */
static dsa_area *tapir_dsa = NULL;

/*
 * Hash function for Oid keys
 */
static uint32
registry_hash_fn(const void *key, size_t keysize, void *arg)
{
	(void)keysize;
	(void)arg;
	return hash_bytes((const unsigned char *)key, sizeof(Oid));
}

/*
 * Compare function for Oid keys
 */
static int
registry_compare_fn(const void *a, const void *b, size_t keysize, void *arg)
{
	Oid oid_a = *(const Oid *)a;
	Oid oid_b = *(const Oid *)b;

	(void)keysize;
	(void)arg;

	if (oid_a < oid_b)
		return -1;
	if (oid_a > oid_b)
		return 1;
	return 0;
}

/*
 * Copy function for Oid keys
 */
static void
registry_copy_fn(void *dest, const void *src, size_t keysize, void *arg)
{
	(void)keysize;
	(void)arg;
	*(Oid *)dest = *(const Oid *)src;
}

/*
 * Fill in dshash parameters for the registry
 */
static void
get_registry_params(dshash_parameters *params)
{
	params->key_size		 = sizeof(Oid);
	params->entry_size		 = sizeof(TpRegistryEntry);
	params->compare_function = registry_compare_fn;
	params->hash_function	 = registry_hash_fn;
	params->copy_function	 = registry_copy_fn;
	params->tranche_id		 = TP_REGISTRY_HASH_TRANCHE_ID;
}

/*
 * Create the registry dshash table
 */
static dshash_table *
registry_create(dsa_area *area)
{
	dshash_parameters params;

	get_registry_params(&params);
	return dshash_create(area, &params, NULL);
}

/*
 * Attach to the registry dshash table
 */
static dshash_table *
registry_attach(dsa_area *area, dshash_table_handle handle)
{
	dshash_parameters params;

	get_registry_params(&params);
	return dshash_attach(area, &params, handle, NULL);
}

/*
 * Request shared memory for the registry
 * Only effective when loaded via shared_preload_libraries
 * Without preloading, registry initializes lazily on first use
 */
void
tp_registry_init(void)
{
	/* Request shared memory for the registry control structure */
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

		/* Initialize handles as invalid - DSA/dshash created on first use */
		tapir_registry->dsa_handle		= DSA_HANDLE_INVALID;
		tapir_registry->registry_handle = DSHASH_HANDLE_INVALID;
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
		dshash_table *registry_hash;
		int			  tranche_id = LWLockNewTrancheId();

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		/* Register the tranche for LWLock debugging/monitoring */
		LWLockRegisterTranche(tranche_id, "pg_textsearch DSA");

		tapir_dsa = dsa_create(tranche_id);
		MemoryContextSwitchTo(oldcontext);

		if (tapir_dsa == NULL)
		{
			LWLockRelease(&tapir_registry->lock);
			elog(ERROR, "Failed to create DSA area");
		}

		/* Pin the DSA to keep it alive across backends */
		dsa_pin(tapir_dsa);

		/* Pin the mapping for this backend */
		dsa_pin_mapping(tapir_dsa);

		/* Store handle for other backends */
		tapir_registry->dsa_handle = dsa_get_handle(tapir_dsa);

		/* Create the registry dshash */
		registry_hash					= registry_create(tapir_dsa);
		tapir_registry->registry_handle = dshash_get_hash_table_handle(
				registry_hash);
		dshash_detach(registry_hash);
	}
	else
	{
		/* DSA exists - attach to it */
		MemoryContext oldcontext;

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
 * Returns true on success (always succeeds with dshash - no limit)
 */
bool
tp_registry_register(
		Oid index_oid, TpSharedIndexState *shared_state, dsa_pointer shared_dp)
{
	dshash_table	*registry_hash;
	TpRegistryEntry *entry;
	bool			 found;

	(void)shared_state; /* Not stored - we use DSA pointer instead */

	/* Ensure DSA and registry are initialized */
	tp_registry_get_dsa();

	if (!tapir_registry ||
		tapir_registry->registry_handle == DSHASH_HANDLE_INVALID)
	{
		elog(ERROR,
			 "Failed to initialize Tapir registry for index %u",
			 index_oid);
	}

	registry_hash =
			registry_attach(tapir_dsa, tapir_registry->registry_handle);
	if (!registry_hash)
		elog(ERROR, "Failed to attach to registry hash table");

	/* Insert or update the entry */
	entry = (TpRegistryEntry *)
			dshash_find_or_insert(registry_hash, &index_oid, &found);
	entry->index_oid	   = index_oid;
	entry->shared_state_dp = shared_dp;
	dshash_release_lock(registry_hash, entry);

	dshash_detach(registry_hash);

	return true;
}

/*
 * Look up an index in the registry
 * Returns the shared state pointer (as DSA pointer cast) or NULL if not found
 */
TpSharedIndexState *
tp_registry_lookup(Oid index_oid)
{
	dshash_table	*registry_hash;
	TpRegistryEntry *entry;
	dsa_pointer		 result = InvalidDsaPointer;

	/* Ensure DSA is initialized */
	tp_registry_get_dsa();

	if (!tapir_registry ||
		tapir_registry->registry_handle == DSHASH_HANDLE_INVALID)
	{
		return NULL;
	}

	registry_hash =
			registry_attach(tapir_dsa, tapir_registry->registry_handle);
	if (!registry_hash)
		return NULL;

	entry = (TpRegistryEntry *)dshash_find(registry_hash, &index_oid, false);
	if (entry)
	{
		result = entry->shared_state_dp;
		dshash_release_lock(registry_hash, entry);
	}

	dshash_detach(registry_hash);

	/* Return DSA pointer cast as TpSharedIndexState pointer
	 * The caller will convert it back */
	return (TpSharedIndexState *)(uintptr_t)result;
}

/*
 * Look up an index's DSA pointer in the registry
 * Returns the DSA pointer if found, InvalidDsaPointer otherwise
 */
dsa_pointer
tp_registry_lookup_dsa(Oid index_oid)
{
	dshash_table	*registry_hash;
	TpRegistryEntry *entry;
	dsa_pointer		 result = InvalidDsaPointer;

	/* Ensure DSA is initialized */
	tp_registry_get_dsa();

	if (!tapir_registry ||
		tapir_registry->registry_handle == DSHASH_HANDLE_INVALID)
	{
		return InvalidDsaPointer;
	}

	registry_hash =
			registry_attach(tapir_dsa, tapir_registry->registry_handle);
	if (!registry_hash)
		return InvalidDsaPointer;

	entry = (TpRegistryEntry *)dshash_find(registry_hash, &index_oid, false);
	if (entry)
	{
		result = entry->shared_state_dp;
		dshash_release_lock(registry_hash, entry);
	}

	dshash_detach(registry_hash);

	return result;
}

/*
 * Get DSA pointer to shared state from the registry
 * Returns the DSA pointer or InvalidDsaPointer if not found
 */
dsa_pointer
tp_registry_get_shared_dp(Oid index_oid)
{
	/* Same implementation as tp_registry_lookup_dsa */
	return tp_registry_lookup_dsa(index_oid);
}

/*
 * Check if an index is registered
 * Returns true if the index is in the registry, false otherwise
 */
bool
tp_registry_is_registered(Oid index_oid)
{
	dshash_table	*registry_hash;
	TpRegistryEntry *entry;
	bool			 result = false;

	/*
	 * Don't try to initialize if registry doesn't exist yet.
	 * This function is called from object access hook which may fire
	 * before any index has been created.
	 */
	if (!tapir_registry)
	{
		tp_registry_shmem_startup();
		if (!tapir_registry)
			return false;
	}

	if (tapir_registry->registry_handle == DSHASH_HANDLE_INVALID)
		return false;

	/* Ensure DSA is attached */
	tp_registry_get_dsa();
	if (!tapir_dsa)
		return false;

	registry_hash =
			registry_attach(tapir_dsa, tapir_registry->registry_handle);
	if (!registry_hash)
		return false;

	entry = (TpRegistryEntry *)dshash_find(registry_hash, &index_oid, false);
	if (entry)
	{
		result = true;
		dshash_release_lock(registry_hash, entry);
	}

	dshash_detach(registry_hash);

	return result;
}

/*
 * Unregister an index from the registry
 * Called when an index is dropped
 */
void
tp_registry_unregister(Oid index_oid)
{
	dshash_table *registry_hash;
	bool		  deleted;

	if (!tapir_registry ||
		tapir_registry->registry_handle == DSHASH_HANDLE_INVALID)
	{
		return;
	}

	/* Ensure DSA is attached */
	tp_registry_get_dsa();
	if (!tapir_dsa)
		return;

	registry_hash =
			registry_attach(tapir_dsa, tapir_registry->registry_handle);
	if (!registry_hash)
		return;

	deleted = dshash_delete_key(registry_hash, &index_oid);
	(void)deleted; /* Ignore if not found */

	dshash_detach(registry_hash);
}

/*
 * Reset the DSA handle in the registry
 *
 * This clears all index entries from the registry hash.
 * Called when the extension is dropped.
 */
void
tp_registry_reset_dsa(void)
{
	dshash_table	 *registry_hash;
	dshash_seq_status status;
	TpRegistryEntry	 *entry;

	if (!tapir_registry ||
		tapir_registry->registry_handle == DSHASH_HANDLE_INVALID)
	{
		return;
	}

	/* Ensure DSA is attached */
	tp_registry_get_dsa();
	if (!tapir_dsa)
		return;

	registry_hash =
			registry_attach(tapir_dsa, tapir_registry->registry_handle);
	if (!registry_hash)
		return;

	/* Delete all entries by iterating */
	dshash_seq_init(&status, registry_hash, true); /* exclusive for deletion */

	while ((entry = (TpRegistryEntry *)dshash_seq_next(&status)) != NULL)
	{
		dshash_delete_current(&status);
	}

	dshash_seq_term(&status);
	dshash_detach(registry_hash);
}
