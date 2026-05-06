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
 */
typedef struct TpGlobalRegistry
{
	LWLock				lock;			 /* Protects initialization */
	dsa_handle			dsa_handle;		 /* Handle for shared DSA area */
	dshash_table_handle registry_handle; /* Handle for the registry dshash */
	pg_atomic_uint64	total_dsa_bytes; /* Tracked global DSA size */

	/*
	 * Running sum of estimated memtable bytes across all indexes.
	 * Updated incrementally via CAS on per-index estimated_bytes
	 * and atomic add/sub here, avoiding expensive registry scans
	 * on the insert hot path.
	 */
	pg_atomic_uint64 estimated_total_bytes;
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

/* DSA memory tracking */
extern uint64 tp_registry_get_total_dsa_bytes(void);
extern void	  tp_registry_update_dsa_counter(void);

/* Memory limit eviction */
extern bool tp_evict_largest_memtable(Oid caller_oid);

/* GUC variable declared in mod.c */
extern int tp_memory_limit;

/*
 * Derived limits from tp_memory_limit (all in bytes, 0 = disabled):
 *   hard  = memory_limit
 *   soft  = memory_limit / 2   (global eviction threshold)
 *   per   = memory_limit / 8   (per-index spill threshold)
 */
static inline uint64
tp_hard_limit_bytes(void)
{
	return (uint64)tp_memory_limit * 1024ULL;
}

static inline uint64
tp_soft_limit_bytes(void)
{
	return (uint64)tp_memory_limit * 512ULL; /* / 2 */
}

static inline uint64
tp_per_index_limit_bytes(void)
{
	return (uint64)tp_memory_limit * 128ULL; /* / 8 */
}

/* Memory estimation */
extern uint64 tp_estimate_memtable_bytes(TpMemtable *memtable);
extern uint64 tp_estimate_total_memtable_bytes(void);

/* Global estimated-bytes counter (maintained incrementally) */
extern void
tp_update_index_estimate(TpSharedIndexState *shared, TpMemtable *memtable);
extern void	  tp_subtract_index_estimate(TpSharedIndexState *shared);
extern uint64 tp_get_estimated_total_bytes(void);
