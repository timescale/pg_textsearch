/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * state.c - Index state management
 *
 * Manages TpLocalIndexState and TpSharedIndexState structures for
 * coordinating index state across backends.
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/generic_xlog.h>
#include <access/heapam.h>
#include <access/relation.h>
#include <access/xact.h>
#include <access/xlog.h>
#include <access/xlogrecovery.h>
#include <catalog/index.h>
#include <executor/executor.h>
#include <lib/dshash.h>
#include <miscadmin.h>
#include <nodes/execnodes.h>
#include <replication/walreceiver.h>
#include <storage/bufmgr.h>
#include <storage/dsm.h>
#include <storage/dsm_registry.h>
#include <storage/ipc.h>
#include <utils/builtins.h>
#include <utils/dsa.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>

#include "access/am.h"
#include "constants.h"
#include "index/metapage.h"
#include "index/registry.h"
#include "index/source.h"
#include "index/state.h"
#include "memtable/cache.h"
#include "memtable/chain_source.h"
#include "memtable/log.h"
#include "segment/io.h"
#include "segment/merge.h"
#include "segment/segment.h"

/* Cache of local index states */
static HTAB *local_state_cache = NULL;

typedef struct LocalStateCacheEntry
{
	Oid				   index_oid;	/* Hash key */
	TpLocalIndexState *local_state; /* Cached local state */
} LocalStateCacheEntry;

/* Registered with before_shmem_exit at most once per backend */
static bool shutdown_hook_registered = false;

/*
 * Spill one cache entry, catching and swallowing any error so the
 * outer loop can continue with the remaining entries.  Split out of
 * tp_shutdown_spill_callback so its PG_TRY doesn't nest in the same
 * lexical scope (which would shadow the outer PG_TRY's locals and
 * trip -Werror=shadow=compatible-local).
 */
static void
tp_shutdown_spill_one(LocalStateCacheEntry *entry)
{
	Relation index_rel = NULL;

	if (entry->local_state == NULL)
		return;

	PG_TRY();
	{
		index_rel = try_index_open(entry->index_oid, RowExclusiveLock);
		if (index_rel != NULL)
		{
			tp_spill_memtable_if_needed(
					index_rel, entry->local_state, TP_MIN_SPILL_PAGES);
			index_close(index_rel, RowExclusiveLock);
			index_rel = NULL;
		}
	}
	PG_CATCH();
	{
		/* Don't leak the per-index LWLock to racing shutdown hooks */
		tp_release_index_lock(entry->local_state);
		FlushErrorState();
		if (index_rel != NULL)
			index_close(index_rel, RowExclusiveLock);
	}
	PG_END_TRY();
}

/*
 * before_shmem_exit hook: spill the memtable of every index this
 * backend has touched, so the on-disk memtable chain doesn't
 * accumulate runt pages that would have to be merge-compacted
 * later.  Skipped on clean client exit (code == 0); fires on any
 * FATAL (cluster shutdown, pg_terminate_backend, etc.).
 */
static void
tp_shutdown_spill_callback(int code, Datum arg pg_attribute_unused())
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;
	bool				  started_txn = false;

	if (local_state_cache == NULL || code == 0)
		return;

	/* catalog access needs a non-aborted transaction */
	if (IsAbortedTransactionBlockState())
		return;

	PG_TRY();
	{
		if (!IsTransactionState())
		{
			StartTransactionCommand();
			started_txn = true;
		}

		hash_seq_init(&status, local_state_cache);
		while ((entry = (LocalStateCacheEntry *)hash_seq_search(&status)) !=
			   NULL)
			tp_shutdown_spill_one(entry);

		if (started_txn)
			CommitTransactionCommand();
	}
	PG_CATCH();
	{
		/* proc_exit still has to complete; swallow and move on */
		if (IsTransactionState())
			AbortCurrentTransaction();
		FlushErrorState();
	}
	PG_END_TRY();
}

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
	ctl.keysize	  = sizeof(Oid);
	ctl.entrysize = sizeof(LocalStateCacheEntry);
	ctl.hcxt	  = TopMemoryContext;

	local_state_cache = hash_create(
			"Tapir Local State Cache",
			8, /* initial size */
			&ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	if (!shutdown_hook_registered)
	{
		before_shmem_exit(tp_shutdown_spill_callback, (Datum)0);
		shutdown_hook_registered = true;
	}
}

/*
 * Get or create a local index state for the given index OID
 *
 * This function:
 * 1. Checks if we already have a cached local state
 * 2. If not, looks up the shared state in the registry
 * 3. Attaches to the shared DSA if needed
 * 4. Creates and caches the local state
 */
TpLocalIndexState *
tp_get_local_index_state(Oid index_oid)
{
	LocalStateCacheEntry *entry;
	TpLocalIndexState	 *local_state;
	TpSharedIndexState	 *shared_state;
	bool				  found;
	dsa_area			 *dsa;

	/* Initialize cache if needed */
	init_local_state_cache();

	/* Check cache first */
	entry = (LocalStateCacheEntry *)
			hash_search(local_state_cache, &index_oid, HASH_FIND, NULL);

	if (entry != NULL)
		return entry->local_state;

	/* Look up shared state in registry */
	shared_state = tp_registry_lookup(index_oid);

	if (shared_state == NULL)
	{
		/*
		 * No registry entry found. This could mean:
		 * 1. The index was just dropped
		 * 2. We're in crash recovery after a restart
		 * 3. The index doesn't exist
		 * 4. The index is being built right now
		 * 5. Backend startup when other backends created the index
		 *
		 * Check if the index exists and needs to be rebuilt.
		 */
		Relation index_rel;
		bool	 index_exists = false;

		PG_TRY();
		{
			index_rel = index_open(index_oid, AccessShareLock);
			if (index_rel != NULL)
			{
				index_exists = true;
				index_close(index_rel, AccessShareLock);
			}
		}
		PG_CATCH();
		{
			/* Index doesn't exist - that's fine */
			FlushErrorState();
		}
		PG_END_TRY();

		if (index_exists)
		{
			/*
			 * Index exists on disk but not in the registry. This can
			 * occur after:
			 * 1. PostgreSQL crash/restart (shared memory was cleared)
			 * 2. Extension reload after DROP/CREATE EXTENSION
			 * 3. Backend startup when other backends created the index
			 *
			 * We rebuild the index state from the on-disk metapage to
			 * recover the memtable and posting lists.
			 */
			local_state = tp_rebuild_index_from_disk(index_oid);
			if (local_state != NULL)
			{
				/*
				 * Rebuilt from disk = pre-existing index.
				 * Override the subtransaction ID so rollback
				 * of an enclosing savepoint won't destroy the
				 * shared state used by all backends.
				 */
				local_state->created_in_subxact = InvalidSubTransactionId;
				return local_state;
			}

			/* Recovery failed - index might be corrupted or stale */
		}

		/* Index not found in registry and doesn't exist on disk */
		return NULL;
	}

	/*
	 * If we reach here, shared_state is set (non-NULL) and is actually a DSA
	 * pointer that needs to be converted to an address.
	 */
	{
		/* shared_state is actually a DSA pointer - need to attach to DSA and
		 * convert to address */
		dsa_pointer shared_dp = (dsa_pointer)(uintptr_t)shared_state;

		/* Get the shared DSA area */
		dsa = tp_registry_get_dsa();

		/* Convert DSA pointer to memory address in this backend */
		shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);

		/* Allocate local state */
		local_state = (TpLocalIndexState *)MemoryContextAlloc(
				TopMemoryContext, sizeof(TpLocalIndexState));
		local_state->shared					 = shared_state;
		local_state->dsa					 = dsa;
		local_state->is_build_mode			 = false; /* Runtime mode */
		local_state->lock_held				 = false;
		local_state->lock_mode				 = 0;
		local_state->terms_added_this_xact	 = 0;
		local_state->docs_since_global_check = 0;
		local_state->created_in_subxact =
				InvalidSubTransactionId; /* pre-existing index */

		/* Cache the local state */
		entry = (LocalStateCacheEntry *)
				hash_search(local_state_cache, &index_oid, HASH_ENTER, &found);
		entry->local_state = local_state;

		return local_state;
	}
}

/*
 * Create a new shared index state and return local state.
 *
 * If `reuse_if_exists` is true and a registry entry for this OID
 * already exists when we go to insert, the just-allocated DSA is
 * freed and we attach to the existing entry instead. This is the
 * mode the cold-start bootstrap path uses, so concurrent rebuilds
 * resolve to the same shared state instead of racing each other.
 *
 * If false, an existing entry is unregistered and replaced — used
 * by the parallel-build completion path on the assumption that any
 * pre-existing entry would be stale (e.g. left behind by a crashed
 * earlier CREATE INDEX with the same OID).
 */
TpLocalIndexState *
tp_create_shared_index_state(Oid index_oid, Oid heap_oid, bool reuse_if_exists)
{
	TpSharedIndexState	 *shared_state;
	TpLocalIndexState	 *local_state;
	TpMemtable			 *memtable;
	dsa_area			 *dsa;
	dsa_pointer			  shared_dp;
	dsa_pointer			  memtable_dp;
	LocalStateCacheEntry *entry;
	bool				  found;

	/* Get the shared DSA area */
	dsa = tp_registry_get_dsa();

	/*
	 * Allocate shared state in DSA.
	 * Use dsa_allocate directly (not tp_dsa_allocate) because shared_state
	 * contains the memory_usage tracker itself. This allocation is not
	 * counted against the index memory limit.
	 */
	shared_dp = dsa_allocate(dsa, sizeof(TpSharedIndexState));
	if (shared_dp == InvalidDsaPointer)
	{
		elog(ERROR,
			 "Failed to allocate DSA memory for shared state (index OID: %u, "
			 "size: %zu)",
			 index_oid,
			 sizeof(TpSharedIndexState));
	}
	shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);

	/* Initialize shared state */
	shared_state->index_oid = index_oid;
	shared_state->heap_oid	= heap_oid;
	pg_atomic_init_u64(&shared_state->estimated_bytes, 0);
	pg_atomic_init_u32(&shared_state->chain_page_count, 0);
	shared_state->is_build_mode = false; /* runtime-mode publication */
	/*
	 * Initialize per-index LWLock + spill_generation in the
	 * freshly DSA-allocated shared_state.  dsa_allocate does
	 * NOT zero memory (reused chunks can hold garbage), so any
	 * field that requires a known initial value must be set
	 * explicitly.  Without these inits a future
	 * LWLockAcquire(&shared->lock) can hang on a stuck spinlock
	 * (PANIC: stuck spinlock detected at LWLockWaitListLock).
	 * Same tranche choices as tp_create_build_index_state for
	 * consistency.
	 */
	LWLockInitialize(&shared_state->lock, TP_TRANCHE_INDEX_LOCK);
	pg_atomic_init_u64(&shared_state->spill_generation, 0);
	memtable_dp = dsa_allocate(dsa, sizeof(TpMemtable));
	if (!DsaPointerIsValid(memtable_dp))
		elog(ERROR, "Failed to allocate memtable in DSA");

	memtable = (TpMemtable *)dsa_get_address(dsa, memtable_dp);
	memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;
	LWLockInitialize(&memtable->apply_lock, TP_TRANCHE_CACHE_APPLY_LOCK);
	LWLockInitialize(&memtable->lock, TP_TRANCHE_CACHE_LOCK);
	memtable->cursor_gen_spill_count = 0;
	memtable->cursor_next_blkno		 = InvalidBlockNumber;
	memtable->cursor_next_off		 = 0;
	pg_atomic_init_u64(&memtable->cursor_seq, 0);
	pg_atomic_init_u64(&memtable->estimated_bytes, 0);

	shared_state->memtable_dp = memtable_dp;

	if (reuse_if_exists)
	{
		/*
		 * Atomic register-or-attach. If a concurrent backend
		 * registered first, free our just-allocated DSA and attach
		 * to its shared state instead — second arrival's bootstrap
		 * still runs (under the per-index LW_EXCLUSIVE), but the
		 * idempotency gate dedups and the atomic-write yields the
		 * same answer.
		 */
		dsa_pointer existing_dp = InvalidDsaPointer;

		if (!tp_registry_register_if_absent(
					index_oid, shared_dp, &existing_dp))
		{
			dsa_free(dsa, memtable_dp);
			dsa_free(dsa, shared_dp);
			shared_dp	 = existing_dp;
			shared_state = (TpSharedIndexState *)
					dsa_get_address(dsa, existing_dp);
		}
	}
	else
	{
		/* Replace any stale entry (e.g. left by a crashed CREATE INDEX). */
		if (tp_registry_lookup(index_oid) != NULL)
			tp_registry_unregister(index_oid);

		if (!tp_registry_register(index_oid, shared_state, shared_dp))
		{
			tp_registry_shmem_startup();
			if (!tp_registry_register(index_oid, shared_state, shared_dp))
			{
				dsa_free(dsa, memtable_dp);
				dsa_free(dsa, shared_dp);
				elog(ERROR, "Failed to register index %u", index_oid);
			}
		}
	}

	/* Create local state for the creating backend */
	local_state = (TpLocalIndexState *)
			MemoryContextAlloc(TopMemoryContext, sizeof(TpLocalIndexState));
	local_state->shared					 = shared_state;
	local_state->dsa					 = dsa;
	local_state->is_build_mode			 = false; /* Runtime mode */
	local_state->lock_held				 = false;
	local_state->lock_mode				 = 0;
	local_state->terms_added_this_xact	 = 0;
	local_state->docs_since_global_check = 0;
	local_state->created_in_subxact		 = GetCurrentSubTransactionId();

	/* Cache the local state */
	init_local_state_cache();
	entry = (LocalStateCacheEntry *)
			hash_search(local_state_cache, &index_oid, HASH_ENTER, &found);
	if (found)
	{
		/* This happens during index rebuild (e.g., VACUUM FULL)
		 * Clean up the old local state before registering the new one */
		if (entry->local_state != NULL)
		{
			/* Don't detach DSA as it's shared */
			pfree(entry->local_state);
		}
	}

	entry->local_state = local_state;

	return local_state;
}

/*
 * Create index state for BUILD mode (CREATE INDEX)
 *
 * Uses a private DSA that is not shared with other backends.
 * This private DSA will be destroyed and recreated on each spill,
 * providing perfect memory reclamation.
 */
TpLocalIndexState *
tp_create_build_index_state(Oid index_oid, Oid heap_oid)
{
	TpSharedIndexState	 *shared_state;
	TpLocalIndexState	 *local_state;
	TpMemtable			 *memtable;
	dsa_area			 *private_dsa;
	dsa_area			 *global_dsa;
	dsa_pointer			  shared_dp;
	dsa_pointer			  memtable_dp;
	LocalStateCacheEntry *entry;
	bool				  found;

	/* Get the global DSA for shared state allocation */
	global_dsa = tp_registry_get_dsa();

	/* Allocate shared state in GLOBAL DSA (for statistics) */
	shared_dp = dsa_allocate(global_dsa, sizeof(TpSharedIndexState));
	if (shared_dp == InvalidDsaPointer)
		elog(ERROR,
			 "Failed to allocate shared state for build (index OID: %u)",
			 index_oid);

	shared_state = (TpSharedIndexState *)
			dsa_get_address(global_dsa, shared_dp);

	/* Initialize shared state */
	shared_state->index_oid = index_oid;
	shared_state->heap_oid	= heap_oid;
	shared_state->memtable_dp =
			InvalidDsaPointer; /* Memtable in private DSA */
	/*
	 * Mark this shared state as build-mode BEFORE we publish it
	 * via tp_registry_register below.  The cross-index eviction
	 * walker (src/memtable/cache.c:evict_walk_cb) reads this
	 * flag lock-free and skips entries where it is true, which
	 * prevents the walker from dereferencing memtable_dp through
	 * the GLOBAL DSA once we publish a PRIVATE-DSA pointer at
	 * "shared_state->memtable_dp = memtable_dp" further down.
	 *
	 * The flag is cleared in tp_finalize_build_mode() under
	 * tp_registry_eviction_mutex EXCL, atomically with swapping
	 * memtable_dp to a global-DSA pointer.
	 */
	shared_state->is_build_mode = true;
	pg_atomic_init_u64(&shared_state->estimated_bytes, 0);
	pg_atomic_init_u32(&shared_state->chain_page_count, 0);

	/*
	 * Initialize per-index LWLock using a fixed tranche ID.
	 * Using a fixed ID avoids exhausting tranche IDs when creating many
	 * indexes (e.g., partitioned tables with 500+ partitions).
	 */
	LWLockInitialize(&shared_state->lock, TP_TRANCHE_INDEX_LOCK);
	pg_atomic_init_u64(&shared_state->spill_generation, 0);

	/* Check if index already registered (rebuild case) */
	if (tp_registry_lookup(index_oid) != NULL)
		tp_registry_unregister(index_oid);

	/* Register in global registry */
	if (!tp_registry_register(index_oid, shared_state, shared_dp))
	{
		tp_registry_shmem_startup();
		if (!tp_registry_register(index_oid, shared_state, shared_dp))
		{
			dsa_free(global_dsa, shared_dp);
			elog(ERROR, "Failed to register index %u", index_oid);
		}
	}

	/*
	 * Create PRIVATE DSA for build.
	 * This DSA is not registered globally - only this backend knows about it.
	 * It will be destroyed and recreated on each spill for perfect
	 * memory reclamation.
	 *
	 * Use a fixed tranche ID to avoid exhausting tranche IDs when creating
	 * many indexes (e.g., partitioned tables with 500+ partitions).
	 */
	private_dsa = dsa_create(TP_TRANCHE_BUILD_DSA);
	if (!private_dsa)
		elog(ERROR, "Failed to create private DSA for index build");

	/* Allocate and initialize memtable in PRIVATE DSA */
	memtable_dp = dsa_allocate(private_dsa, sizeof(TpMemtable));
	if (!DsaPointerIsValid(memtable_dp))
		elog(ERROR, "Failed to allocate memtable in private DSA");

	memtable = (TpMemtable *)dsa_get_address(private_dsa, memtable_dp);
	memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;
	LWLockInitialize(&memtable->apply_lock, TP_TRANCHE_CACHE_APPLY_LOCK);
	LWLockInitialize(&memtable->lock, TP_TRANCHE_CACHE_LOCK);
	memtable->cursor_gen_spill_count = 0;
	memtable->cursor_next_blkno		 = InvalidBlockNumber;
	memtable->cursor_next_off		 = 0;
	pg_atomic_init_u64(&memtable->cursor_seq, 0);
	pg_atomic_init_u64(&memtable->estimated_bytes, 0);

	/* Store memtable pointer in shared state for memtable access */
	shared_state->memtable_dp = memtable_dp;

	/* Create local state pointing to PRIVATE DSA */
	local_state = (TpLocalIndexState *)
			MemoryContextAlloc(TopMemoryContext, sizeof(TpLocalIndexState));
	local_state->shared					 = shared_state;
	local_state->dsa					 = private_dsa; /* PRIVATE DSA */
	local_state->is_build_mode			 = true;		/* BUILD MODE */
	local_state->lock_held				 = false;
	local_state->lock_mode				 = 0;
	local_state->terms_added_this_xact	 = 0;
	local_state->docs_since_global_check = 0;
	local_state->created_in_subxact		 = GetCurrentSubTransactionId();

	/* Cache the local state */
	init_local_state_cache();
	entry = (LocalStateCacheEntry *)
			hash_search(local_state_cache, &index_oid, HASH_ENTER, &found);
	if (found && entry->local_state != NULL)
		pfree(entry->local_state);

	entry->local_state = local_state;

	return local_state;
}

/*
 * Finalize build mode and transition to runtime mode
 *
 * This is called at the end of CREATE INDEX. It:
 * 1. Destroys the private DSA (returns all memory to OS)
 * 2. Attaches to the global shared DSA
 * 3. Initializes a fresh memtable in the global DSA
 * 4. Sets is_build_mode = false for runtime operation
 *
 * After this, the index is ready for normal concurrent access.
 */
void
tp_finalize_build_mode(TpLocalIndexState *local_state)
{
	dsa_area   *global_dsa;
	TpMemtable *memtable;
	dsa_pointer memtable_dp;
	LWLock	   *eviction_mutex;

	Assert(local_state != NULL);
	Assert(local_state->is_build_mode);

	/*
	 * Attach to the global shared DSA for runtime operation.
	 * This is the same DSA used by all backends for concurrent access.
	 *
	 * NOTE: we do NOT detach the private DSA yet.  The cross-index
	 * eviction walker (src/memtable/cache.c:evict_walk_cb) skips
	 * entries where shared->is_build_mode is true, so as long as
	 * is_build_mode remains true the private memtable_dp is
	 * safe.  We only detach after the swap+clear below.
	 */
	global_dsa = tp_registry_get_dsa();
	if (!global_dsa)
		elog(ERROR, "Failed to get global DSA for runtime mode");

	/*
	 * Allocate a fresh memtable in the global DSA.
	 * This memtable will be shared with other backends.
	 */
	memtable_dp = dsa_allocate(global_dsa, sizeof(TpMemtable));
	if (!DsaPointerIsValid(memtable_dp))
		elog(ERROR, "Failed to allocate memtable in global DSA");

	memtable = (TpMemtable *)dsa_get_address(global_dsa, memtable_dp);
	memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;
	LWLockInitialize(&memtable->apply_lock, TP_TRANCHE_CACHE_APPLY_LOCK);
	LWLockInitialize(&memtable->lock, TP_TRANCHE_CACHE_LOCK);
	memtable->cursor_gen_spill_count = 0;
	memtable->cursor_next_blkno		 = InvalidBlockNumber;
	memtable->cursor_next_off		 = 0;
	pg_atomic_init_u64(&memtable->cursor_seq, 0);
	pg_atomic_init_u64(&memtable->estimated_bytes, 0);

	/*
	 * Publish the new global memtable_dp and clear is_build_mode
	 * atomically with respect to the cross-index eviction walker.
	 * The walker holds tp_registry_eviction_mutex EXCL across its
	 * entire scan, so taking it EXCL here gives readers a single
	 * coherent transition:
	 *
	 *   BEFORE: is_build_mode=true,  memtable_dp=<PRIVATE-DSA ptr>
	 *   AFTER:  is_build_mode=false, memtable_dp=<GLOBAL-DSA ptr>
	 *
	 * Lock order matches evict_largest (eviction_mutex outermost),
	 * so no inversion vs the rest of the eviction subsystem.
	 */
	eviction_mutex = tp_registry_eviction_mutex();
	if (eviction_mutex != NULL)
		LWLockAcquire(eviction_mutex, LW_EXCLUSIVE);

	local_state->shared->memtable_dp   = memtable_dp;
	local_state->shared->is_build_mode = false;

	if (eviction_mutex != NULL)
		LWLockRelease(eviction_mutex);

	/*
	 * Now that no other backend can reach the private DSA via the
	 * registry, detach it.  This returns ALL build-time memory to
	 * the OS.  After build the memtable should be empty (all data
	 * spilled to disk).
	 */
	if (local_state->dsa)
	{
		dsa_detach(local_state->dsa);
		local_state->dsa = NULL;
	}

	local_state->dsa = global_dsa;

	/* Transition to runtime mode (local-state flag) */
	local_state->is_build_mode = false;
}

/*
 * Clean up build mode state on transaction abort
 *
 * This is called from the transaction callback when a transaction aborts.
 * If we were in the middle of a CREATE INDEX (build mode), we need to:
 * 1. Detach from the private DSA (which destroys it since no other refs)
 * 2. Clean up the shared state from the registry
 * 3. Remove from local cache
 *
 * This prevents memory leaks when CREATE INDEX is aborted.
 */
void
tp_cleanup_build_mode_on_abort(void)
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;
	dsa_area			 *global_dsa;

	if (local_state_cache == NULL)
		return;

	global_dsa = tp_registry_get_dsa();

	hash_seq_init(&status, local_state_cache);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		TpLocalIndexState *local_state = entry->local_state;

		if (local_state == NULL)
			continue;

		if (!local_state->is_build_mode)
			continue;

		/*
		 * Detach from private DSA. Since this is a private DSA with no other
		 * attachments, dsa_detach will destroy it and return memory to OS.
		 */
		if (local_state->dsa != NULL && local_state->dsa != global_dsa)
		{
			dsa_detach(local_state->dsa);
			local_state->dsa = NULL;
		}

		/*
		 * Clean up shared state from registry. The shared state was allocated
		 * in the global DSA, so we need to free it there.
		 *
		 * Take tp_registry_eviction_mutex EXCL across the
		 * unregister + dsa_free so the cross-index eviction
		 * walker (src/memtable/cache.c:tp_cache_evict_largest)
		 * cannot deref a half-freed shared_state.  We unregister
		 * FIRST so a future walker can't even find the entry,
		 * then free under the same mutex so any walker that is
		 * currently iterating completes before we recycle the
		 * memory.  Lock order matches evict_largest
		 * (eviction_mutex outermost).
		 */
		if (local_state->shared != NULL)
		{
			Oid			index_oid	   = local_state->shared->index_oid;
			dsa_pointer shared_dp	   = tp_registry_lookup_dsa(index_oid);
			LWLock	   *eviction_mutex = tp_registry_eviction_mutex();

			if (eviction_mutex != NULL)
				LWLockAcquire(eviction_mutex, LW_EXCLUSIVE);

			/* Unregister first so no new walker can find us */
			tp_registry_unregister(index_oid);

			if (DsaPointerIsValid(shared_dp) && global_dsa != NULL)
			{
				/* Free shared state from global DSA */
				dsa_free(global_dsa, shared_dp);
			}

			if (eviction_mutex != NULL)
				LWLockRelease(eviction_mutex);

			local_state->shared = NULL;
		}

		/* Free local state */
		pfree(local_state);
		entry->local_state = NULL;
	}
}

/*
 * Clean up index state on subtransaction abort
 *
 * Called from the SubXactCallback when a subtransaction aborts
 * (e.g., ROLLBACK TO SAVEPOINT). This handles two issues:
 *
 * 1. Registry/shared memory leak: If CREATE INDEX completed within
 *    the subtransaction, the OAT_DROP hook won't fire during
 *    subtransaction abort, so we must clean up manually.
 *
 * 2. LWLock tracking desync: AbortSubTransaction calls
 *    LWLockReleaseAll(), releasing all locks including those
 *    acquired before the savepoint. We must reset our lock_held
 *    tracking for ALL entries to match.
 */
void
tp_cleanup_subxact_abort(SubTransactionId mySubid)
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;
	dsa_area			 *global_dsa;

	if (local_state_cache == NULL)
		return;

	global_dsa = tp_registry_get_dsa();

	hash_seq_init(&status, local_state_cache);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		TpLocalIndexState *ls = entry->local_state;

		if (ls == NULL)
			continue;

		/*
		 * Reset lock tracking for ALL entries, not just ones
		 * from the aborting subtransaction. LWLockReleaseAll()
		 * releases every lock held by this backend.
		 */
		ls->lock_held = false;
		ls->lock_mode = 0;

		/* Only clean up state created in the aborting subxact */
		if (ls->created_in_subxact != mySubid)
			continue;

		/*
		 * Reset bulk load counter only for entries being
		 * destroyed. Resetting surviving entries would suppress
		 * the PRE_COMMIT spill check for terms already in the
		 * memtable from the outer transaction.
		 */
		ls->terms_added_this_xact = 0;

		if (ls->is_build_mode)
		{
			/*
			 * Build mode: private DSA not shared with anyone.
			 * Detach destroys it and returns memory to OS.
			 */
			if (ls->dsa != NULL && ls->dsa != global_dsa)
			{
				dsa_detach(ls->dsa);
				ls->dsa = NULL;
			}
		}

		/*
		 * Free shared state from global DSA and unregister.
		 *
		 * Take tp_registry_eviction_mutex EXCL across the
		 * unregister + dsa_free (and, for runtime mode, the
		 * memtable free + cache clear) so the cross-index
		 * eviction walker can't observe a half-freed shared
		 * state.  We unregister FIRST: once the registry no
		 * longer references this oid, no new walker can find
		 * it, and any walker currently iterating completes
		 * before we recycle the memory.  Lock order matches
		 * evict_largest (eviction_mutex outermost).
		 *
		 * NOTE: the runtime branch's tp_cache_clear is safe
		 * under the eviction_mutex; it only takes per-memtable
		 * locks (cache.apply_lock / cache.lock), which are
		 * acquired BELOW eviction_mutex in the canonical lock
		 * order documented at src/memtable/cache.c.
		 */
		if (ls->shared != NULL)
		{
			Oid			index_oid	   = ls->shared->index_oid;
			dsa_pointer shared_dp	   = tp_registry_lookup_dsa(index_oid);
			LWLock	   *eviction_mutex = tp_registry_eviction_mutex();

			if (eviction_mutex != NULL)
				LWLockAcquire(eviction_mutex, LW_EXCLUSIVE);

			/* Unregister first so no new walker can find us */
			tp_registry_unregister(index_oid);

			if (!ls->is_build_mode && global_dsa != NULL)
			{
				/*
				 * Runtime mode: the in-memory cache (see
				 * docs/memtable_cache.md) may have populated
				 * the dshash tables hanging off the
				 * TpMemtable; drop them first so dsa_free on
				 * the TpMemtable allocation does not leak the
				 * dshash internals.  Safe with an empty
				 * cache: tp_cache_clear is a no-op when both
				 * handles are INVALID.
				 *
				 * Subxact abort doesn't acquire cache.lock
				 * itself: this path is unwinding an aborted
				 * CREATE-INDEX-like subtransaction whose
				 * shared state we just unregistered.
				 */
				TpMemtable *mt = NULL;

				if (DsaPointerIsValid(ls->shared->memtable_dp))
				{
					mt = (TpMemtable *)dsa_get_address(
							global_dsa, ls->shared->memtable_dp);
					tp_cache_clear(global_dsa, mt);
					dsa_free(global_dsa, ls->shared->memtable_dp);
				}
			}

			if (DsaPointerIsValid(shared_dp) && global_dsa != NULL)
				dsa_free(global_dsa, shared_dp);

			if (eviction_mutex != NULL)
				LWLockRelease(eviction_mutex);

			ls->shared = NULL;
		}

		pfree(ls);
		entry->local_state = NULL;
	}
}

/*
 * Promote subtransaction state on subtransaction commit
 *
 * When a subtransaction commits (RELEASE SAVEPOINT), states created
 * within it are "promoted" to the parent subtransaction. This ensures
 * they get cleaned up if the parent later aborts.
 */
void
tp_promote_subxact_states(
		SubTransactionId mySubid, SubTransactionId parentSubid)
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;

	if (local_state_cache == NULL)
		return;

	hash_seq_init(&status, local_state_cache);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		TpLocalIndexState *ls = entry->local_state;

		if (ls == NULL)
			continue;

		if (ls->created_in_subxact == mySubid)
			ls->created_in_subxact = parentSubid;
	}
}

/*
 * Clean up shared memory allocations for an index
 *
 * This is called when an index is dropped. We free the DSA allocations
 * but keep the DSA area itself since it's shared by all indices.
 */
void
tp_cleanup_index_shared_memory(Oid index_oid)
{
	dsa_area			 *dsa;
	dsa_pointer			  shared_dp;
	TpSharedIndexState	 *shared_state;
	LocalStateCacheEntry *entry = NULL;
	bool				  found;

	/*
	 * Release any per-index LWLock this backend still holds for
	 * this index BEFORE we free the DSA memory that backs the
	 * lock.  Without this step, a chain_source or build that
	 * leaks the lock (e.g., via an error path before its close()
	 * runs) would leave a dangling held_lwlocks[] entry pointing
	 * into freed shared memory; the end-of-xact LWLockReleaseAll()
	 * would then trip an assertion in debug/sanitizer builds and
	 * potentially corrupt LWLock accounting in production.
	 *
	 * Look up the local_state up front for this lookup; do NOT
	 * remove it from the cache yet — the later block below will
	 * dispose of it after the DSA frees.
	 */
	if (local_state_cache != NULL)
	{
		entry = hash_search(local_state_cache, &index_oid, HASH_FIND, &found);
		if (found && entry != NULL && entry->local_state != NULL &&
			entry->local_state->lock_held)
			tp_release_index_lock(entry->local_state);
	}
	entry = NULL;

	/* Look up the DSA pointer in registry */
	shared_dp = tp_registry_lookup_dsa(index_oid);

	if (!DsaPointerIsValid(shared_dp))
	{
		/* Still unregister even if no shared state found */
		tp_registry_unregister(index_oid);
		return; /* Nothing to clean up */
	}

	/* Get the shared DSA area */
	dsa = tp_registry_get_dsa();

	/* Get shared state */
	shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);

	/*
	 * The in-memory cache (see docs/memtable_cache.md) may have
	 * populated the dshash tables hanging off the TpMemtable; drop
	 * them first so dsa_free on the TpMemtable allocation does not
	 * leak the dshash internals.  Safe with an empty cache:
	 * tp_cache_clear is a no-op when both handles are INVALID.
	 *
	 * DROP INDEX runs under AccessExclusiveLock on the index, so no
	 * concurrent backend can be reading the cache here; we do not
	 * acquire cache.lock.
	 *
	 * Memtable-cache eviction (docs/memtable_cache.md §"Memory cap
	 * (3 tiers)") accesses victim shared states by DSA pointer
	 * without holding the index relation lock.  Take the global
	 * eviction mutex EXCL across the unregister + dsa_free so a
	 * concurrent evict_largest cannot deref a victim->lock that we
	 * are about to free.  Unregister FIRST so no new walker can
	 * find the entry, then free under the same mutex so any walker
	 * currently iterating completes before we recycle the memory.
	 * The mutex order is global before per-index, matching
	 * evict_largest's acquire sequence.
	 */
	LWLockAcquire(tp_registry_eviction_mutex(), LW_EXCLUSIVE);

	/* Unregister first so no new walker can find us */
	tp_registry_unregister(index_oid);

	if (DsaPointerIsValid(shared_state->memtable_dp))
	{
		TpMemtable *mt = (TpMemtable *)
				dsa_get_address(dsa, shared_state->memtable_dp);

		tp_cache_clear(dsa, mt);
		dsa_free(dsa, shared_state->memtable_dp);
	}

	/* Free shared_state */
	dsa_free(dsa, shared_dp);

	LWLockRelease(tp_registry_eviction_mutex());

	/* Clean up local state if we have it cached */
	if (local_state_cache != NULL)
	{
		entry = hash_search(local_state_cache, &index_oid, HASH_FIND, &found);
		if (found && entry != NULL && entry->local_state != NULL)
		{
			TpLocalIndexState *ls = entry->local_state;

			/* Remove from cache first, before detaching DSA */
			hash_search(local_state_cache, &index_oid, HASH_REMOVE, &found);

			/* Don't detach DSA - it's shared and still in use by registry */
			/* Just nullify the reference */
			ls->dsa	   = NULL;
			ls->shared = NULL;

			/* Free the local state */
			pfree(ls);
		}
	}
}

/*
 * Rebuild index state from disk on the first backend access of an
 * index whose registry entry is empty (server restart, hot
 * standby cold start, PITR-recovered cluster).
 *
 * Post-#374: the durable record of the in-flight memtable
 * lives entirely on the index relation's chain pages.
 * Standard GenericXLog replay restores both the chain pages and
 * the metapage's `memtable_head_blkno`/`memtable_tail_blkno`
 * pointers before any backend gets here, so this function only
 * has to (re)register shared state for lock coordination and
 * seed the chain-page counter used by the auto-spill heuristic.
 *
 * The flow at a high level:
 *
 *   1. validate metapage magic
 *   2. tp_create_shared_index_state with reuse_if_exists=true
 *      (atomic register-or-attach; concurrent rebuilds resolve
 *      to the same shared state)
 *   3. walk the on-disk chain to seed `chain_page_count`
 *
 * No WAL drain and no recovery-time corpus rebuild are needed:
 * the on-disk metapage + chain pages + segments already encode
 * every committed insert.  The in-memory memtable cache
 * (docs/memtable_cache.md) is derived state and lazily built on
 * the first query after rebuild; readers consult the chain source
 * for in-flight statistics in the meantime.
 */
TpLocalIndexState *
tp_rebuild_index_from_disk(Oid index_oid)
{
	Relation		   index_rel;
	TpIndexMetaPage	   early_metap;
	TpLocalIndexState *local_state;
	Oid				   heap_oid;
	TpDataSource	  *chain_src;

	/* Open the index relation */
	index_rel = index_open(index_oid, AccessShareLock);
	if (index_rel == NULL)
	{
		elog(WARNING, "Could not open index %u for recovery", index_oid);
		return NULL;
	}

	heap_oid = index_rel->rd_index->indrelid;

	/*
	 * Read the metapage early so we can validate the magic before
	 * registering shared state.  Magic is set at index creation and
	 * never changes, so this check is stable across replay.
	 */
	early_metap = tp_get_metapage(index_rel);
	if (early_metap == NULL)
	{
		index_close(index_rel, AccessShareLock);
		elog(WARNING, "Could not read metapage for index %u", index_oid);
		return NULL;
	}

	if (early_metap->magic != TP_METAPAGE_MAGIC)
	{
		uint32 found_magic = early_metap->magic;

		index_close(index_rel, AccessShareLock);
		pfree(early_metap);
		elog(WARNING,
			 "Invalid magic number in metapage for index %u: "
			 "expected 0x%08X, found 0x%08X",
			 index_oid,
			 TP_METAPAGE_MAGIC,
			 found_magic);
		return NULL;
	}
	pfree(early_metap);

	/*
	 * Register shared state up front.  Post-#374, durable state
	 * lives entirely on disk (metapage + chain pages + segment
	 * pages).  No docid-page replay or memtable reconstruction
	 * needed — the chain pages are themselves the durable record
	 * of unspilled documents, and standard GenericXLog replay has
	 * already brought them up to date by the time any backend
	 * reaches this path.
	 */
	local_state = tp_create_shared_index_state(
			index_oid, heap_oid, /* reuse_if_exists */ true);
	if (local_state == NULL)
	{
		index_close(index_rel, AccessShareLock);
		return NULL;
	}

	/*
	 * Seed the on-disk chain page counter so the auto-spill
	 * threshold isn't blind to pages that pre-existed in the
	 * index when this backend (or this shmem segment) was
	 * started.  Corpus statistics (total_docs / total_len) live
	 * entirely on the metapage and need no shmem seed.
	 */
	tp_acquire_index_lock(local_state, LW_EXCLUSIVE);

	chain_src =
			tp_memtable_chain_source_create(local_state, index_rel, NULL, 0);
	if (chain_src != NULL)
	{
		uint32 chain_pages;

		chain_pages = tp_memtable_chain_source_page_count(chain_src);
		tp_source_close(chain_src);

		/*
		 * The writer always increments chain_page_count strictly
		 * under SHARED, but we're inside tp_rebuild_index_from_disk's
		 * LW_EXCLUSIVE (acquired above), so we can atomically
		 * initialize it without racing the writer.
		 */
		pg_atomic_write_u32(
				&local_state->shared->chain_page_count, chain_pages);
	}

	tp_release_index_lock(local_state);
	index_close(index_rel, AccessShareLock);

	return local_state;
}

/*
 * Helper function to get memtable from local index state
 * Canonical implementation used by all modules
 */
TpMemtable *
get_memtable(TpLocalIndexState *local_state)
{
	if (!local_state || !local_state->shared || !local_state->dsa)
		return NULL;

	if (!DsaPointerIsValid(local_state->shared->memtable_dp))
		return NULL;

	return (TpMemtable *)dsa_get_address(
			local_state->dsa, local_state->shared->memtable_dp);
}

/*
 * Acquire the per-index lock if not already held by this backend.
 * Ensures memory consistency on NUMA systems through LWLock's
 * built-in memory barriers.
 */
void
tp_acquire_index_lock(TpLocalIndexState *local_state, LWLockMode mode)
{
	Assert(local_state != NULL);
	Assert(local_state->shared != NULL);
	Assert(mode == LW_SHARED || mode == LW_EXCLUSIVE);

	/* If we already hold the lock, check mode compatibility */
	if (local_state->lock_held)
	{
		/*
		 * If we hold exclusive lock, we're good regardless of requested mode.
		 * If we hold shared lock and request shared, we're also good.
		 * But if we hold shared and need exclusive, we must upgrade.
		 */
		if (local_state->lock_mode == LW_EXCLUSIVE ||
			(local_state->lock_mode == LW_SHARED && mode == LW_SHARED))
		{
			return; /* Already have sufficient lock */
		}

		/*
		 * Need to upgrade from shared to exclusive.
		 * This is tricky and can deadlock, so we'll release and re-acquire.
		 * In practice, this shouldn't happen as writers should request
		 * exclusive from the start.
		 */
		elog(WARNING,
			 "Upgrading index lock from shared to exclusive - "
			 "potential deadlock risk");

		LWLockRelease(&local_state->shared->lock);
		local_state->lock_held = false;
	}

	/* Acquire the lock */
	LWLockAcquire(&local_state->shared->lock, mode);
	local_state->lock_held = true;
	local_state->lock_mode = mode;

	/*
	 * The LWLockAcquire provides acquire semantics (memory barrier),
	 * ensuring we see all writes from the previous lock holder.
	 */
}

/*
 * Release the per-index lock if held
 */
void
tp_release_index_lock(TpLocalIndexState *local_state)
{
	if (!local_state || !local_state->lock_held)
		return;

	Assert(local_state->shared != NULL);

	/*
	 * Double-check that PostgreSQL thinks we hold the lock.
	 * This can prevent crashes if our lock tracking gets out of sync
	 * (e.g., during error recovery).
	 */
	if (!LWLockHeldByMe(&local_state->shared->lock))
	{
		/* Our tracking was wrong - fix it and return */
		local_state->lock_held = false;
		local_state->lock_mode = 0;
		return;
	}

	/*
	 * The LWLockRelease provides release semantics (memory barrier),
	 * ensuring our writes are visible to the next lock acquirer.
	 */
	LWLockRelease(&local_state->shared->lock);
	local_state->lock_held = false;
	local_state->lock_mode = 0;
}

/*
 * Release all index locks held by this backend.
 * This is called at transaction end via the transaction callback.
 */
void
tp_release_all_index_locks(void)
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;

	/* Nothing to do if cache not initialized */
	if (local_state_cache == NULL)
		return;

	/* Iterate through all cached local states */
	hash_seq_init(&status, local_state_cache);
	while ((entry = (LocalStateCacheEntry *)hash_seq_search(&status)) != NULL)
	{
		if (entry->local_state && entry->local_state->lock_held)
			tp_release_index_lock(entry->local_state);
	}
}

/*
 * Check if any index should spill to disk due to bulk load threshold.
 * Spill is triggered when terms added this transaction exceeds threshold.
 *
 * Note: memtable_pages_threshold is checked in real-time via
 * tp_auto_spill_if_needed() after each document insert.
 *
 * This is called at PRE_COMMIT via the transaction callback in mod.c.
 */
void
tp_bulk_load_spill_check(void)
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;

	/* Standby is read-only; spill is primary-only. */
	if (RecoveryInProgress())
		return;

	/* Nothing to do if cache not initialized or threshold disabled */
	if (local_state_cache == NULL)
		return;
	if (tp_bulk_load_threshold <= 0)
		return;

	/* Iterate through all cached local states */
	hash_seq_init(&status, local_state_cache);
	while ((entry = (LocalStateCacheEntry *)hash_seq_search(&status)) != NULL)
	{
		TpLocalIndexState *local_state = entry->local_state;
		Relation		   index_rel;
		bool			   index_open_failed = false;

		if (!local_state || !local_state->shared)
			continue;

		/* Check bulk load threshold */
		if (local_state->terms_added_this_xact < tp_bulk_load_threshold)
			continue;

		/*
		 * Acquire exclusive lock — no lock is held at
		 * PRE_COMMIT since per-operation locking releases
		 * after each insert.
		 */
		tp_acquire_index_lock(local_state, LW_EXCLUSIVE);

		/* Open the index relation */
		PG_TRY();
		{
			index_rel = index_open(
					local_state->shared->index_oid, RowExclusiveLock);
		}
		PG_CATCH();
		{
			/* Index might have been dropped */
			FlushErrorState();
			index_open_failed = true;
		}
		PG_END_TRY();

		if (index_open_failed)
		{
			tp_release_index_lock(local_state);
			continue;
		}

		/* Unified spill path. */
		(void)tp_do_spill(local_state, index_rel, NULL);

		index_close(index_rel, RowExclusiveLock);
		tp_release_index_lock(local_state);
	}
}

/*
 * Reset bulk load counters for all cached indexes.
 * Called at transaction end (COMMIT/ABORT) via the transaction callback.
 */
void
tp_reset_bulk_load_counters(void)
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;

	/* Nothing to do if cache not initialized */
	if (local_state_cache == NULL)
		return;

	/* Iterate through all cached local states and reset counters */
	hash_seq_init(&status, local_state_cache);
	while ((entry = (LocalStateCacheEntry *)hash_seq_search(&status)) != NULL)
	{
		if (entry->local_state)
		{
			entry->local_state->terms_added_this_xact = 0;
			/* Note: docs_since_global_check is NOT reset here.
			 * It is an amortization counter that must persist
			 * across transactions so that the global soft limit
			 * check fires for single-row auto-commit INSERTs. */
		}
	}
}
