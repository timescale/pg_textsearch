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

		/* Initialize handles as invalid - DSA/dshash created on first use */
		tapir_registry->dsa_handle		= DSA_HANDLE_INVALID;
		tapir_registry->registry_handle = DSHASH_HANDLE_INVALID;
		pg_atomic_init_u64(&tapir_registry->total_dsa_bytes, 0);
		pg_atomic_init_u64(&tapir_registry->estimated_total_bytes, 0);
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

		/* Initialize DSA byte counter */
		pg_atomic_write_u64(
				&tapir_registry->total_dsa_bytes,
				dsa_get_total_size(tapir_dsa));

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
 * Read the current global DSA byte counter.
 * Returns 0 if registry is not initialized.
 */
uint64
tp_registry_get_total_dsa_bytes(void)
{
	if (!tapir_registry)
		return 0;
	return pg_atomic_read_u64(&tapir_registry->total_dsa_bytes);
}

/*
 * Re-sync the global DSA byte counter from dsa_get_total_size().
 * Called after operations that may change DSA size.
 *
 * This is a simple atomic write of the current DSA segment
 * reservation. A racing backend may briefly overwrite with a
 * slightly stale value, but this is acceptable because:
 * (a) the hard limit check is approximate by design, and
 * (b) the next call from any backend will correct it.
 *
 * We intentionally do NOT use a monotonic CAS here, because
 * dsa_get_total_size() CAN decrease when dsa_trim() frees
 * empty segments. If we only allowed increases, the hard
 * limit would become a permanent high-water mark that blocks
 * inserts forever after a single overshoot.
 */
void
tp_registry_update_dsa_counter(void)
{
	uint64 new_size;

	if (!tapir_registry)
		return;

	if (!tapir_dsa)
		tp_registry_get_dsa();

	if (!tapir_dsa)
		return;

	new_size = dsa_get_total_size(tapir_dsa);
	pg_atomic_write_u64(&tapir_registry->total_dsa_bytes, new_size);
}

/*
 * Estimate memory consumption for a memtable based on its
 * statistics.  Derived empirically from MS MARCO profiling
 * (8.8M passages, 234M postings).  The dominant cost is
 * ~28 bytes per posting entry (DSA allocation overhead +
 * TpPostingEntry).  Term overhead covers the dshash entry,
 * DSA-allocated string, and posting list header.
 */
#define TP_BYTES_PER_POSTING	28
#define TP_BYTES_PER_TERM_FIXED 216

uint64
tp_estimate_memtable_bytes(TpMemtable *memtable)
{
	if (!memtable)
		return 0;

	return (uint64)pg_atomic_read_u64(&memtable->total_postings) *
				   TP_BYTES_PER_POSTING +
		   (uint64)pg_atomic_read_u64(&memtable->num_terms) *
				   TP_BYTES_PER_TERM_FIXED +
		   (uint64)pg_atomic_read_u64(&memtable->total_term_len);
}

/*
 * Sum estimated memory across all registered memtables.
 */
uint64
tp_estimate_total_memtable_bytes(void)
{
	dshash_table	 *registry_hash;
	dshash_seq_status seq;
	TpRegistryEntry	 *entry;
	uint64			  total = 0;

	if (!tapir_registry)
		return 0;

	/* Ensure DSA is attached (lazy init for fresh backends) */
	if (!tapir_dsa)
		tp_registry_get_dsa();

	if (!tapir_dsa || tapir_registry->registry_handle == DSHASH_HANDLE_INVALID)
		return 0;

	registry_hash =
			registry_attach(tapir_dsa, tapir_registry->registry_handle);

	dshash_seq_init(&seq, registry_hash, false);

	while ((entry = dshash_seq_next(&seq)) != NULL)
	{
		TpSharedIndexState *shared;
		TpMemtable		   *memtable;

		if (!DsaPointerIsValid(entry->shared_state_dp))
			continue;

		shared = (TpSharedIndexState *)
				dsa_get_address(tapir_dsa, entry->shared_state_dp);

		if (!DsaPointerIsValid(shared->memtable_dp))
			continue;

		memtable = (TpMemtable *)
				dsa_get_address(tapir_dsa, shared->memtable_dp);

		total += tp_estimate_memtable_bytes(memtable);
	}

	dshash_seq_term(&seq);
	dshash_detach(registry_hash);

	return total;
}

/*
 * Update the per-index estimated_bytes atomically via CAS, and
 * adjust the global estimated_total_bytes counter by the delta.
 *
 * Safe for concurrent callers on the same index: the CAS ensures
 * exactly one caller applies each transition.  Losers retry and
 * see the winner's value, producing a smaller (or zero) delta.
 */
void
tp_update_index_estimate(TpSharedIndexState *shared, TpMemtable *memtable)
{
	uint64 new_est;
	uint64 old_est;

	if (!shared || !memtable || !tapir_registry)
		return;

	new_est = tp_estimate_memtable_bytes(memtable);

	old_est = pg_atomic_read_u64(&shared->estimated_bytes);
	if (old_est == new_est)
		return;

	while (!pg_atomic_compare_exchange_u64(
			&shared->estimated_bytes, &old_est, new_est))
	{
		if (old_est == new_est)
			return;
	}

	/*
	 * CAS succeeded: old_est holds the previous value, new_est
	 * is now stored.  Apply the signed delta to the global sum.
	 */
	if (new_est > old_est)
		pg_atomic_fetch_add_u64(
				&tapir_registry->estimated_total_bytes, new_est - old_est);
	else
		pg_atomic_fetch_sub_u64(
				&tapir_registry->estimated_total_bytes, old_est - new_est);
}

/*
 * Subtract this index's estimated bytes from the global counter
 * and zero the per-index value.  Called before freeing shared
 * state (DROP INDEX) or after clearing a memtable.
 */
void
tp_subtract_index_estimate(TpSharedIndexState *shared)
{
	uint64 old_est;

	if (!shared || !tapir_registry)
		return;

	old_est = pg_atomic_read_u64(&shared->estimated_bytes);
	if (old_est == 0)
		return;

	while (!pg_atomic_compare_exchange_u64(
			&shared->estimated_bytes, &old_est, 0))
	{
		if (old_est == 0)
			return;
	}

	pg_atomic_fetch_sub_u64(&tapir_registry->estimated_total_bytes, old_est);
}

/*
 * Read the global estimated total (O(1), no registry scan).
 */
uint64
tp_get_estimated_total_bytes(void)
{
	if (!tapir_registry)
		return 0;

	return pg_atomic_read_u64(&tapir_registry->estimated_total_bytes);
}

/* --------------------------------------------------------
 * Eviction: spill largest memtable to free DSA memory
 * --------------------------------------------------------
 */

#define TP_MAX_EVICTION_CANDIDATES 8

typedef struct TpEvictionCandidate
{
	Oid	   index_oid;
	uint64 estimated_bytes;
} TpEvictionCandidate;

/*
 * Collect eviction candidates sorted by estimated_bytes descending.
 *
 * Note: We read memtable fields without holding the per-index lock.
 * This is safe because DSA memory is never unmapped (only returned
 * to the DSA freelist), so the dereference cannot fault.  Values
 * may be stale if another backend is concurrently spilling, but
 * the actual eviction is protected by LWLockConditionalAcquire.
 */
static int
find_eviction_candidates(TpEvictionCandidate *candidates, int max_candidates)
{
	dshash_table	 *registry_hash;
	dshash_seq_status seq;
	TpRegistryEntry	 *entry;
	int				  count = 0;

	if (!tapir_registry || !tapir_dsa ||
		tapir_registry->registry_handle == DSHASH_HANDLE_INVALID)
		return 0;

	registry_hash =
			registry_attach(tapir_dsa, tapir_registry->registry_handle);

	dshash_seq_init(&seq, registry_hash, false);

	while ((entry = dshash_seq_next(&seq)) != NULL)
	{
		TpSharedIndexState *shared;
		TpMemtable		   *memtable;
		uint64				est;

		if (!DsaPointerIsValid(entry->shared_state_dp))
			continue;

		shared = (TpSharedIndexState *)
				dsa_get_address(tapir_dsa, entry->shared_state_dp);

		if (!DsaPointerIsValid(shared->memtable_dp))
			continue;

		memtable = (TpMemtable *)
				dsa_get_address(tapir_dsa, shared->memtable_dp);

		est = tp_estimate_memtable_bytes(memtable);
		if (est == 0)
			continue;

		/* Insertion sort into candidates (small N) */
		if (count < max_candidates ||
			est > candidates[count - 1].estimated_bytes)
		{
			int pos = (count < max_candidates) ? count : count - 1;
			int i;

			candidates[pos].index_oid		= entry->index_oid;
			candidates[pos].estimated_bytes = est;

			/* Bubble up to maintain descending order */
			for (i = pos; i > 0; i--)
			{
				if (candidates[i].estimated_bytes >
					candidates[i - 1].estimated_bytes)
				{
					TpEvictionCandidate tmp = candidates[i];
					candidates[i]			= candidates[i - 1];
					candidates[i - 1]		= tmp;
				}
				else
					break;
			}

			if (count < max_candidates)
				count++;
		}
	}

	dshash_seq_term(&seq);
	dshash_detach(registry_hash);

	return count;
}

/*
 * Attempt to spill the largest memtable to free DSA memory.
 *
 * caller_oid: the calling backend's own index OID (lock already
 * held) or InvalidOid if no lock is held.
 *
 * Uses LWLockConditionalAcquire to avoid deadlocks when the
 * caller already holds a per-index lock.
 *
 * Returns true if a memtable was successfully spilled.
 */
bool
tp_evict_largest_memtable(Oid caller_oid)
{
	TpEvictionCandidate candidates[TP_MAX_EVICTION_CANDIDATES];
	int					num_candidates;
	int					i;

	num_candidates =
			find_eviction_candidates(candidates, TP_MAX_EVICTION_CANDIDATES);

	if (num_candidates == 0)
		return false;

	for (i = 0; i < num_candidates; i++)
	{
		Oid				   target_oid = candidates[i].index_oid;
		uint64			   target_est = candidates[i].estimated_bytes;
		TpLocalIndexState *target_state;
		Relation		   index_rel	 = NULL;
		BlockNumber		   segment_root	 = InvalidBlockNumber;
		bool			   lock_was_ours = false;

		target_state = tp_get_local_index_state(target_oid);
		if (!target_state)
			continue;

		/*
		 * If this is the caller's own index, the lock is already
		 * held. Otherwise, use conditional acquire to avoid
		 * deadlocks.
		 */
		if (target_oid == caller_oid && target_state->lock_held)
		{
			lock_was_ours = true;
		}
		else
		{
			if (!LWLockConditionalAcquire(
						&target_state->shared->lock, LW_EXCLUSIVE))
				continue;

			target_state->lock_held = true;
			target_state->lock_mode = LW_EXCLUSIVE;
		}

		/*
		 * Try to lock the index relation without blocking.
		 * If another transaction holds a conflicting lock,
		 * skip this candidate to avoid deadlock (we already
		 * hold an LWLock on the caller's index).
		 */
		if (!ConditionalLockRelationOid(target_oid, RowExclusiveLock))
		{
			if (!lock_was_ours)
				tp_release_index_lock(target_state);
			continue;
		}

		/*
		 * Open the index (lock already held), spill, and
		 * clean up.  On ERROR the transaction aborts and
		 * tp_release_all_index_locks() + Postgres lock
		 * manager handle cleanup.
		 */
		index_rel = index_open(target_oid, NoLock);

		segment_root = tp_write_segment(target_state, index_rel);

		if (segment_root != InvalidBlockNumber)
		{
			tp_clear_memtable(target_state);
			tp_clear_docid_pages(index_rel);
			tp_link_l0_chain_head(index_rel, segment_root);
			tp_maybe_compact_level(index_rel, 0);

			elog(LOG,
				 "pg_textsearch: memory pressure, "
				 "spilled memtable for index %u "
				 "(est " UINT64_FORMAT " kB)",
				 target_oid,
				 (uint64)(target_est / 1024));
		}

		if (!lock_was_ours)
			tp_release_index_lock(target_state);
		index_close(index_rel, RowExclusiveLock);

		/* Update the counter after spill */
		tp_registry_update_dsa_counter();

		/*
		 * If spill produced a segment, we succeeded.
		 * If the memtable was empty (stale candidate),
		 * try the next candidate.
		 */
		if (segment_root != InvalidBlockNumber)
			return true;
		/* else continue to next candidate */
	}

	return false;
}
