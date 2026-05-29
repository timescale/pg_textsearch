/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * registry.c - Global registry mapping index OIDs to shared state
 *
 * Uses a dshash (dynamic shared hash table) for O(1) lookups and no
 * limit on the number of indexes (beyond available memory).
 */
#include <postgres.h>

#include <access/hash.h>
#include <access/relation.h>
#include <lib/dshash.h>
#include <miscadmin.h>
#include <storage/ipc.h>
#include <storage/lmgr.h>
#include <storage/lock.h>
#include <storage/lwlock.h>
#include <storage/shmem.h>
#include <utils/dsa.h>
#include <utils/memutils.h>

#include "access/am.h"
#include "index/metapage.h"
#include "index/registry.h"
#include "segment/io.h"
#include "segment/merge.h"
#include "segment/segment.h"

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
 * Called during shared_preload_libraries processing
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

		/*
		 * Initialize the registry lock using fixed tranche ID.
		 * Using a fixed ID avoids exhausting tranche IDs when creating many
		 * indexes (e.g., partitioned tables with 500+ partitions).
		 */
		LWLockInitialize(&tapir_registry->lock, TP_TRANCHE_REGISTRY);

		/*
		 * In-memory memtable cache fields.  eviction_mutex
		 * serializes evict_largest invocations across backends;
		 * estimated_total_bytes is the live sum of per-index
		 * cache estimated_bytes.  See
		 * docs/memtable_cache.md §"Memory cap (3 tiers)".
		 */
		LWLockInitialize(
				&tapir_registry->eviction_mutex, TP_TRANCHE_EVICTION_MUTEX);
		pg_atomic_init_u64(&tapir_registry->estimated_total_bytes, 0);

		/* Initialize handles as invalid - DSA/dshash created on first use */
		tapir_registry->dsa_handle		= DSA_HANDLE_INVALID;
		tapir_registry->registry_handle = DSHASH_HANDLE_INVALID;
	}

	LWLockRelease(AddinShmemInitLock);

	/* Register the lock tranches */
	LWLockRegisterTranche(tapir_registry->lock.tranche, "tapir_registry");
	LWLockRegisterTranche(
			tapir_registry->eviction_mutex.tranche, "tapir_cache_eviction");
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

	/* Registry must be initialized via shared_preload_libraries */
	if (!tapir_registry)
		elog(ERROR,
			 "Tapir registry not initialized. "
			 "Is pg_textsearch in shared_preload_libraries?");

	/* Check if DSA exists, create if needed */
	LWLockAcquire(&tapir_registry->lock, LW_EXCLUSIVE);

	if (tapir_registry->dsa_handle == DSA_HANDLE_INVALID)
	{
		/* First backend - create the DSA */
		MemoryContext oldcontext;
		dshash_table *registry_hash;

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);

		/*
		 * Create DSA using fixed tranche ID. Using a fixed ID avoids
		 * exhausting tranche IDs when creating many indexes (e.g.,
		 * partitioned tables with 500+ partitions).
		 */
		tapir_dsa = dsa_create(TP_TRANCHE_GLOBAL_DSA);
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
			elog(ERROR,
				 "Failed to attach to pg_textsearch "
				 "shared DSA");
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
 * Atomic register-if-absent. Returns true if we created a fresh
 * entry (caller's shared_dp is now in the registry); false if an
 * entry already existed (in which case *existing_dp is set to its
 * shared_state_dp and the caller should free its own allocation).
 */
bool
tp_registry_register_if_absent(
		Oid index_oid, dsa_pointer shared_dp, dsa_pointer *existing_dp)
{
	dshash_table	*registry_hash;
	TpRegistryEntry *entry;
	bool			 found;

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

	entry = (TpRegistryEntry *)
			dshash_find_or_insert(registry_hash, &index_oid, &found);
	if (found)
	{
		if (existing_dp)
			*existing_dp = entry->shared_state_dp;
	}
	else
	{
		entry->index_oid	   = index_oid;
		entry->shared_state_dp = shared_dp;
	}
	dshash_release_lock(registry_hash, entry);
	dshash_detach(registry_hash);

	return !found;
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
	 * If registry is not initialized, no indexes can be registered.
	 * This function is called from object access hook which may fire
	 * before any index has been created.
	 */
	if (!tapir_registry)
		return false;

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
 * Cache memory accounting accessors.  Both return pointers into
 * the shmem registry control struct, valid for the lifetime of
 * the postmaster.  See docs/memtable_cache.md §"Memory cap
 * (3 tiers)".
 */
pg_atomic_uint64 *
tp_registry_estimated_total_bytes(void)
{
	Assert(tapir_registry != NULL);
	return &tapir_registry->estimated_total_bytes;
}

LWLock *
tp_registry_eviction_mutex(void)
{
	Assert(tapir_registry != NULL);
	return &tapir_registry->eviction_mutex;
}

void
tp_registry_walk(TpRegistryWalkCb cb, void *ctx)
{
	dshash_table	 *registry_hash;
	dshash_seq_status status;
	TpRegistryEntry	 *entry;

	Assert(cb != NULL);

	tp_registry_get_dsa();
	if (!tapir_registry ||
		tapir_registry->registry_handle == DSHASH_HANDLE_INVALID)
		return;

	registry_hash =
			registry_attach(tapir_dsa, tapir_registry->registry_handle);
	if (!registry_hash)
		return;

	dshash_seq_init(&status, registry_hash, false);
	while ((entry = (TpRegistryEntry *)dshash_seq_next(&status)) != NULL)
	{
		bool stop;

		stop = cb(entry->index_oid, entry->shared_state_dp, ctx);
		if (stop)
		{
			dshash_seq_term(&status);
			dshash_detach(registry_hash);
			return;
		}
	}
	dshash_seq_term(&status);
	dshash_detach(registry_hash);
}
