/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * registry.h - Global registry for shared index states
 *
 * Uses a dshash (dynamic shared hash table) to map index OIDs to their
 * shared state DSA pointers. This allows unlimited indexes (bounded only
 * by available memory) with O(1) lookup performance.
 */
#pragma once

#include <postgres.h>

#include <lib/dshash.h>
#include <port/atomics.h>
#include <storage/lwlock.h>
#include <utils/dsa.h>

#include "index/state.h"

/*
 * LWLock tranche ID for the registry dshash is defined in constants.h
 * as TP_TRANCHE_REGISTRY. We use fixed tranche IDs to avoid exhausting
 * tranche IDs when creating many indexes (e.g., partitioned tables with
 * 500+ partitions).
 */
#include "constants.h"
#define TP_REGISTRY_HASH_TRANCHE_ID TP_TRANCHE_REGISTRY

/*
 * Registry entry stored in dshash
 * The key is the first field (index_oid), value is shared_state_dp
 */
typedef struct TpRegistryEntry
{
	Oid			index_oid;		 /* Hash key - must be first */
	dsa_pointer shared_state_dp; /* DSA pointer to TpSharedIndexState */
} TpRegistryEntry;

/*
 * Global registry control structure stored in shared memory.
 * The actual entries are in a dshash stored in DSA.
 *
 * The eviction_mutex and estimated_total_bytes fields are part
 * of the in-memory memtable cache memory-cap protocol; see
 * docs/memtable_cache.md §"Memory cap (3 tiers)".  estimated_total_bytes
 * tracks the sum of TpMemtable.estimated_bytes across all registered
 * caches in this shmem segment; eviction_mutex serializes
 * tp_cache_evict_largest invocations (and DROP-time shared-state
 * teardown) so a victim's TpSharedIndexState cannot be dsa_freed
 * while another backend is inspecting it.
 */
typedef struct TpGlobalRegistry
{
	LWLock				lock;			 /* Protects initialization */
	dsa_handle			dsa_handle;		 /* Handle for shared DSA area */
	dshash_table_handle registry_handle; /* Handle for the registry dshash */
	LWLock				eviction_mutex;	 /* Serializes cache eviction */
	pg_atomic_uint64	estimated_total_bytes; /* Σ per-index est bytes */
} TpGlobalRegistry;

/* Registry management functions */
extern void tp_registry_init(void);
extern void tp_registry_shmem_startup(void);

/* DSA management */
extern dsa_area *tp_registry_get_dsa(void);

/* Registry operations */
extern bool tp_registry_register(
		Oid					index_oid,
		TpSharedIndexState *shared_state,
		dsa_pointer			shared_dp);
/*
 * Register `shared_dp` for `index_oid` only if no entry exists.
 * Returns true if a fresh entry was created, false if an existing
 * entry was found (in which case `*existing_dp` is set to the
 * existing shared_state_dp and the caller is expected to free its
 * own allocation and use the existing entry).
 */
extern bool tp_registry_register_if_absent(
		Oid index_oid, dsa_pointer shared_dp, dsa_pointer *existing_dp);
extern TpSharedIndexState *tp_registry_lookup(Oid index_oid);
extern dsa_pointer		   tp_registry_lookup_dsa(Oid index_oid);
extern bool				   tp_registry_is_registered(Oid index_oid);
extern void				   tp_registry_unregister(Oid index_oid);

/*
 * Cache memory accounting helpers.  Increment/decrement the
 * registry's estimated_total_bytes alongside per-index updates.
 * Internally these route through TpGlobalRegistry; cache code that
 * mutates TpMemtable.estimated_bytes MUST go through these so the
 * global counter stays in lockstep with the per-index counter.
 *
 * tp_registry_eviction_mutex returns the address of the global
 * eviction LWLock; tp_cache_evict_largest is the only legitimate
 * caller, and tp_cleanup_index_shared_memory takes it EXCL before
 * dsa_freeing a victim shared state.
 */
extern pg_atomic_uint64 *tp_registry_estimated_total_bytes(void);
extern LWLock			*tp_registry_eviction_mutex(void);

/*
 * Callback-based registry iterator.  For each registered index,
 * invokes `cb(oid, shared_state_dp, ctx)`.  Stops early when the
 * callback returns true.  Returns true if the callback ever
 * returned true, false otherwise.  Bucket locks are held while
 * the callback runs; the callback MUST NOT acquire arbitrary
 * LWLocks (the dshash partition lock is held).  Reading fields
 * via dsa_get_address from the supplied DSA pointer is safe.
 *
 * Used by tp_cache_evict_largest to scan for the largest-bytes
 * cache.  Reads only — no entry mutation from within the
 * callback.
 */
typedef bool (*TpRegistryWalkCb)(
		Oid index_oid, dsa_pointer shared_dp, void *ctx);
extern void tp_registry_walk(TpRegistryWalkCb cb, void *ctx);
