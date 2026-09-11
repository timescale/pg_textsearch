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

static void
init_memtable(TpMemtable *memtable)
{
	memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;
	LWLockInitialize(&memtable->apply_lock, TP_TRANCHE_CACHE_APPLY_LOCK);
	LWLockInitialize(&memtable->lock, TP_TRANCHE_CACHE_LOCK);
	memtable->cursor_gen_spill_count = 0;
	memtable->cursor_next_blkno		 = InvalidBlockNumber;
	memtable->cursor_next_off		 = 0;
	memset(&memtable->cursor_locator, 0, sizeof(memtable->cursor_locator));
	memtable->cursor_locator_valid = false;
	pg_atomic_init_u64(&memtable->cursor_seq, 0);
	pg_atomic_init_u64(&memtable->estimated_bytes, 0);
}

static dsa_pointer
allocate_memtable(dsa_area *dsa)
{
	dsa_pointer memtable_dp = dsa_allocate(dsa, sizeof(TpMemtable));
	TpMemtable *memtable;

	if (!DsaPointerIsValid(memtable_dp))
		elog(ERROR, "Failed to allocate memtable in DSA");

	memtable = (TpMemtable *)dsa_get_address(dsa, memtable_dp);
	init_memtable(memtable);

	return memtable_dp;
}

static void
init_local_state(
		TpLocalIndexState  *local_state,
		TpSharedIndexState *shared_state,
		dsa_pointer			shared_dp,
		dsa_area		   *dsa)
{
	local_state->shared						= shared_state;
	local_state->shared_dp					= shared_dp;
	local_state->dsa						= dsa;
	local_state->build_created_shared_state = false;
	local_state->lock_held					= false;
	local_state->lock_mode					= 0;
	local_state->terms_added_this_xact		= 0;
	local_state->docs_since_global_check	= 0;
	local_state->created_in_subxact			= InvalidSubTransactionId;
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
	shared_state = tp_registry_lookup(
			tp_registry_key(MyDatabaseId, index_oid));

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

		index_rel = try_index_open(index_oid, AccessShareLock);
		if (index_rel != NULL)
		{
			index_exists = true;
			index_close(index_rel, AccessShareLock);
		}

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
				return local_state;

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
		init_local_state(local_state, shared_state, shared_dp, dsa);

		/* Cache the local state */
		entry = (LocalStateCacheEntry *)
				hash_search(local_state_cache, &index_oid, HASH_ENTER, &found);
		entry->local_state = local_state;

		return local_state;
	}
}

/*
 * Register a fresh runtime state or attach to the existing state for the
 * same database-qualified index identity.  REINDEX must never replace the
 * shared allocation because wrappers retained by other backends keep its
 * address.  The calling backend's wrapper is replaced below.
 */
static TpLocalIndexState *
create_or_attach_index_state(
		Oid index_oid, Oid heap_oid, SubTransactionId index_create_subid)
{
	TpRegistryKey		  key = tp_registry_key(MyDatabaseId, index_oid);
	TpSharedIndexState	 *shared_state;
	TpLocalIndexState	 *local_state;
	TpLocalIndexState	 *old_local_state = NULL;
	dsa_area			 *dsa;
	dsa_pointer			  shared_dp;
	dsa_pointer			  memtable_dp;
	dsa_pointer			  existing_dp = InvalidDsaPointer;
	LocalStateCacheEntry *entry;
	bool				  found;

	dsa = tp_registry_get_dsa();

	shared_dp = dsa_allocate(dsa, sizeof(TpSharedIndexState));
	if (!DsaPointerIsValid(shared_dp))
		elog(ERROR,
			 "Failed to allocate DSA memory for shared state (index OID: %u, "
			 "size: %zu)",
			 index_oid,
			 sizeof(TpSharedIndexState));

	shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);
	shared_state->index_oid = index_oid;
	shared_state->heap_oid	= heap_oid;
	pg_atomic_init_u64(&shared_state->estimated_bytes, 0);
	pg_atomic_init_u32(&shared_state->chain_page_count, 0);
	pg_atomic_init_u32(&shared_state->chain_page_count_needs_reseed, 0);
	pg_atomic_init_u32(&shared_state->chain_page_count_spc_oid, InvalidOid);
	pg_atomic_init_u32(&shared_state->chain_page_count_db_oid, InvalidOid);
	pg_atomic_init_u32(
			&shared_state->chain_page_count_rel_number, InvalidRelFileNumber);
	LWLockInitialize(&shared_state->lock, TP_TRANCHE_INDEX_LOCK);
	pg_atomic_init_u64(&shared_state->spill_generation, 0);
	memtable_dp				  = allocate_memtable(dsa);
	shared_state->memtable_dp = memtable_dp;

	if (!tp_registry_register_if_absent(key, shared_dp, &existing_dp))
	{
		dsa_free(dsa, memtable_dp);
		dsa_free(dsa, shared_dp);
		shared_dp	 = existing_dp;
		shared_state = (TpSharedIndexState *)dsa_get_address(dsa, existing_dp);
	}

	/*
	 * The wrapper cache is backend-local and can outlive a committed DROP
	 * performed by another backend.  The registry state resolved above is
	 * authoritative; never inspect the old wrapper's potentially stale
	 * shared pointer.  Release only the known-live shared lock, if this
	 * backend already acquired it earlier in the transaction.
	 */
	if (LWLockHeldByMe(&shared_state->lock))
		LWLockRelease(&shared_state->lock);

	local_state = (TpLocalIndexState *)
			MemoryContextAlloc(TopMemoryContext, sizeof(TpLocalIndexState));
	init_local_state(local_state, shared_state, shared_dp, dsa);
	if (index_create_subid != InvalidSubTransactionId)
	{
		local_state->build_created_shared_state = true;
		local_state->created_in_subxact			= index_create_subid;
	}

	init_local_state_cache();
	entry = (LocalStateCacheEntry *)
			hash_search(local_state_cache, &index_oid, HASH_ENTER, &found);
	if (found)
		old_local_state = entry->local_state;

	/*
	 * REINDEX replaces this backend's wrapper while retaining the same
	 * registry allocation.  Carry transaction-local scalar accounting
	 * forward only when the saved DSA identity matches the authoritative
	 * state resolved above.  Never dereference old_local_state->shared.
	 */
	if (old_local_state != NULL && old_local_state->shared_dp == shared_dp)
	{
		local_state->terms_added_this_xact =
				old_local_state->terms_added_this_xact;
		local_state->docs_since_global_check =
				old_local_state->docs_since_global_check;
	}

	entry->local_state = local_state;
	if (old_local_state != NULL)
		pfree(old_local_state);

	return local_state;
}

TpLocalIndexState *
tp_create_shared_index_state(
		Oid index_oid, Oid heap_oid, SubTransactionId index_create_subid)
{
	return create_or_attach_index_state(
			index_oid, heap_oid, index_create_subid);
}

TpLocalIndexState *
tp_create_build_index_state(
		Oid index_oid, Oid heap_oid, SubTransactionId index_create_subid)
{
	return create_or_attach_index_state(
			index_oid, heap_oid, index_create_subid);
}

/*
 * A successful serial or parallel build replaces the on-disk relfilenode.
 * Discard the derived cache in place while preserving the registry entry,
 * per-index LWLock, TpMemtable allocation, and memtable_dp.
 */
void
tp_finalize_build_mode(TpLocalIndexState *local_state)
{
	TpMemtable *memtable;

	Assert(local_state != NULL);
	Assert(local_state->shared != NULL);
	Assert(local_state->dsa != NULL);
	Assert(DsaPointerIsValid(local_state->shared->memtable_dp));

	tp_acquire_index_lock(local_state, LW_EXCLUSIVE);
	memtable = (TpMemtable *)dsa_get_address(
			local_state->dsa, local_state->shared->memtable_dp);
	LWLockAcquire(&memtable->apply_lock, LW_EXCLUSIVE);
	LWLockAcquire(&memtable->lock, LW_EXCLUSIVE);
	tp_cache_clear(local_state->dsa, memtable);
	pg_atomic_add_fetch_u64(&local_state->shared->spill_generation, 1);
	pg_atomic_write_u32(
			&local_state->shared->chain_page_count_needs_reseed, 1);
	pg_atomic_write_u32(&local_state->shared->chain_page_count, 0);
	LWLockRelease(&memtable->lock);
	LWLockRelease(&memtable->apply_lock);
	tp_release_index_lock(local_state);
}

static void
cleanup_owned_build_state(LocalStateCacheEntry *entry, dsa_area *global_dsa)
{
	TpLocalIndexState *local_state = entry->local_state;
	TpRegistryKey	   key;
	dsa_pointer		   shared_dp;
	LWLock			  *eviction_mutex;

	Assert(local_state != NULL);
	Assert(local_state->build_created_shared_state);
	Assert(local_state->shared != NULL);

	key		  = tp_registry_key(MyDatabaseId, local_state->shared->index_oid);
	shared_dp = tp_registry_lookup_dsa(key);
	eviction_mutex = tp_registry_eviction_mutex();

	if (eviction_mutex != NULL)
		LWLockAcquire(eviction_mutex, LW_EXCLUSIVE);

	tp_registry_unregister(key);
	if (DsaPointerIsValid(shared_dp) && global_dsa != NULL)
	{
		TpMemtable *memtable = (TpMemtable *)
				dsa_get_address(global_dsa, local_state->shared->memtable_dp);

		tp_cache_clear(global_dsa, memtable);
		dsa_free(global_dsa, local_state->shared->memtable_dp);
		dsa_free(global_dsa, shared_dp);
	}

	if (eviction_mutex != NULL)
		LWLockRelease(eviction_mutex);

	pfree(local_state);
	entry->local_state = NULL;
}

/*
 * Abort removes only a shared state registered by this transaction's
 * initial CREATE INDEX.  REINDEX reuses pre-existing state; failure before
 * finalization leaves its cache untouched, while abort after finalization
 * may leave the stable cache empty.
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

		if (!local_state->build_created_shared_state)
			continue;

		cleanup_owned_build_state(entry, global_dsa);
	}
}

void
tp_commit_build_states(void)
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;

	if (local_state_cache == NULL)
		return;

	hash_seq_init(&status, local_state_cache);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		if (entry->local_state != NULL)
		{
			entry->local_state->build_created_shared_state = false;
			entry->local_state->created_in_subxact = InvalidSubTransactionId;
		}
	}
}

bool
tp_has_initial_create_ownership(void)
{
	HASH_SEQ_STATUS		  status;
	LocalStateCacheEntry *entry;

	if (local_state_cache == NULL)
		return false;

	hash_seq_init(&status, local_state_cache);
	while ((entry = hash_seq_search(&status)) != NULL)
	{
		if (entry->local_state != NULL &&
			entry->local_state->build_created_shared_state)
			return true;
	}

	return false;
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

		/* Only process build state created in the aborting subxact. */
		if (ls->created_in_subxact != mySubid)
			continue;

		if (ls->build_created_shared_state)
		{
			ls->terms_added_this_xact = 0;
			cleanup_owned_build_state(entry, global_dsa);
		}
		else
		{
			ls->created_in_subxact = InvalidSubTransactionId;
		}
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
static void
cleanup_index_shared_memory(TpRegistryKey key, bool cleanup_local_state)
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
	Assert(!cleanup_local_state || key.database_oid == MyDatabaseId);

	if (cleanup_local_state && local_state_cache != NULL)
	{
		entry = hash_search(
				local_state_cache, &key.index_oid, HASH_FIND, &found);
		if (found && entry != NULL && entry->local_state != NULL &&
			entry->local_state->lock_held)
			tp_release_index_lock(entry->local_state);
	}
	entry = NULL;

	/* Look up the DSA pointer in registry */
	LWLockAcquire(tp_registry_eviction_mutex(), LW_EXCLUSIVE);

	shared_dp = tp_registry_lookup_dsa(key);

	if (!DsaPointerIsValid(shared_dp))
	{
		/* Still unregister even if no shared state found */
		tp_registry_unregister(key);
		LWLockRelease(tp_registry_eviction_mutex());
		return; /* Nothing to clean up */
	}

	/* Get the shared DSA area */
	dsa = tp_registry_get_dsa();

	/* Get shared state */
	shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);

	/*
	 * The in-memory cache may have populated the dshash tables
	 * hanging off the TpMemtable; drop them first so dsa_free on
	 * the TpMemtable allocation does not leak the dshash internals.
	 * Safe with an empty cache:
	 * tp_cache_clear is a no-op when both handles are INVALID.
	 *
	 * DROP INDEX runs under AccessExclusiveLock on the index, so no
	 * concurrent backend can be reading the cache here; we do not
	 * acquire cache.lock.
	 *
	 * Memtable-cache eviction accesses victim shared states by DSA
	 * pointer without holding the index relation lock.  Take the
	 * global eviction mutex EXCL across the unregister + dsa_free
	 * so a concurrent evict_largest cannot deref a victim->lock
	 * that we are about to free.  Unregister FIRST so no new walker
	 * can find the entry, then free under the same mutex so any
	 * walker currently iterating completes before we recycle the
	 * memory.  The mutex order is global before per-index, matching
	 * evict_largest's acquire sequence.
	 */
	/* Unregister first so no new walker can find us */
	tp_registry_unregister(key);

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
	if (cleanup_local_state && local_state_cache != NULL)
	{
		entry = hash_search(
				local_state_cache, &key.index_oid, HASH_FIND, &found);
		if (found && entry != NULL && entry->local_state != NULL)
		{
			TpLocalIndexState *ls = entry->local_state;

			/* Remove from cache first, before detaching DSA */
			hash_search(
					local_state_cache, &key.index_oid, HASH_REMOVE, &found);

			/* Don't detach DSA - it's shared and still in use by registry */
			/* Just nullify the reference */
			ls->dsa	   = NULL;
			ls->shared = NULL;

			/* Free the local state */
			pfree(ls);
		}
	}
}

void
tp_cleanup_index_shared_memory(Oid index_oid)
{
	cleanup_index_shared_memory(
			tp_registry_key(MyDatabaseId, index_oid), true);
}

/*
 * Clean up every shared allocation qualified by a successfully dropped
 * database.  DROP DATABASE cannot target the current connection, so this
 * path deliberately leaves the backend-local wrapper cache untouched.
 */
void
tp_cleanup_database_shared_memory(Oid database_oid)
{
	TpRegistryKey *keys;
	Size		   key_count;
	Size		   i;

	if (!OidIsValid(database_oid))
		return;

	Assert(database_oid != MyDatabaseId);

	key_count = tp_registry_collect_database_keys(database_oid, &keys);
	for (i = 0; i < key_count; i++)
		cleanup_index_shared_memory(keys[i], false);

	if (keys != NULL)
		pfree(keys);
}

void
tp_set_chain_page_count_for_relation(
		TpLocalIndexState *local_state, Relation index_rel, uint32 chain_pages)
{
	RelFileLocator locator;

	Assert(local_state != NULL);
	Assert(local_state->shared != NULL);
	Assert(local_state->lock_held);
	Assert(local_state->lock_mode == LW_EXCLUSIVE);
	Assert(index_rel != NULL);

	locator = index_rel->rd_locator;
	pg_atomic_write_u32(&local_state->shared->chain_page_count, chain_pages);
	pg_atomic_write_u32(
			&local_state->shared->chain_page_count_spc_oid, locator.spcOid);
	pg_atomic_write_u32(
			&local_state->shared->chain_page_count_db_oid, locator.dbOid);
	pg_atomic_write_u32(
			&local_state->shared->chain_page_count_rel_number,
			locator.relNumber);
	pg_atomic_write_u32(
			&local_state->shared->chain_page_count_needs_reseed, 0);
}

static bool
chain_page_count_matches_relation(
		TpLocalIndexState *local_state, Relation index_rel)
{
	RelFileLocator locator = index_rel->rd_locator;

	Assert(local_state != NULL);
	Assert(local_state->shared != NULL);
	Assert(index_rel != NULL);

	if (pg_atomic_read_u32(
				&local_state->shared->chain_page_count_needs_reseed) != 0)
		return false;

	return pg_atomic_read_u32(
				   &local_state->shared->chain_page_count_spc_oid) ==
				   locator.spcOid &&
		   pg_atomic_read_u32(&local_state->shared->chain_page_count_db_oid) ==
				   locator.dbOid &&
		   pg_atomic_read_u32(
				   &local_state->shared->chain_page_count_rel_number) ==
				   locator.relNumber;
}

static void
reseed_chain_page_count_locked(
		TpLocalIndexState *local_state, Relation index_rel)
{
	TpDataSource *chain_src;
	uint32		  chain_pages = 0;

	Assert(local_state != NULL);
	Assert(local_state->shared != NULL);
	Assert(local_state->lock_held);
	Assert(local_state->lock_mode == LW_EXCLUSIVE);
	Assert(index_rel != NULL);

	chain_src =
			tp_memtable_chain_source_create(local_state, index_rel, NULL, 0);
	if (chain_src != NULL)
	{
		chain_pages = tp_memtable_chain_source_page_count(chain_src);
		tp_source_close(chain_src);
	}

	tp_set_chain_page_count_for_relation(local_state, index_rel, chain_pages);
}

/*
 * Rebuild finalization cannot know whether commit or rollback will select the
 * new or old relfilenode.  Defer page-count reconstruction until a threshold
 * consumer has the current Relation, then recount under the per-index lock.
 */
void
tp_reseed_chain_page_count_if_needed(
		TpLocalIndexState *local_state, Relation index_rel)
{
	bool acquired_lock = false;

	if (local_state == NULL || local_state->shared == NULL ||
		index_rel == NULL)
		return;

	if (chain_page_count_matches_relation(local_state, index_rel))
		return;

	if (!local_state->lock_held)
	{
		tp_acquire_index_lock(local_state, LW_EXCLUSIVE);
		acquired_lock = true;
	}
	else
		Assert(local_state->lock_mode == LW_EXCLUSIVE);

	if (!chain_page_count_matches_relation(local_state, index_rel))
		reseed_chain_page_count_locked(local_state, index_rel);

	if (acquired_lock)
		tp_release_index_lock(local_state);
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
 * every committed insert.  The in-memory memtable cache is
 * derived state and lazily built on the first query after rebuild;
 * readers consult the chain source for in-flight statistics in
 * the meantime.
 */
TpLocalIndexState *
tp_rebuild_index_from_disk(Oid index_oid)
{
	Relation		   index_rel;
	TpIndexMetaPage	   early_metap;
	TpLocalIndexState *local_state;
	Oid				   heap_oid;
	SubTransactionId   index_create_subid;

	/* Open the index relation */
	index_rel = index_open(index_oid, AccessShareLock);
	if (index_rel == NULL)
	{
		elog(WARNING, "Could not open index %u for recovery", index_oid);
		return NULL;
	}

	heap_oid		   = index_rel->rd_index->indrelid;
	index_create_subid = index_rel->rd_createSubid;

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
			index_oid, heap_oid, index_create_subid);
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
	reseed_chain_page_count_locked(local_state, index_rel);
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

	/* ereport(ERROR) resets the holdoff before PG_FINALLY cleanup. */
	if (InterruptHoldoffCount == 0)
		HOLD_INTERRUPTS();

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

		if (!local_state || !local_state->shared)
			continue;

		/* Check bulk load threshold */
		if (local_state->terms_added_this_xact < tp_bulk_load_threshold)
			continue;

		/*
		 * Open the relation before taking the per-index LWLock: relation
		 * and catalog access can block, and must stay outside the
		 * per-index lock ordering domain.  No per-index lock is held on
		 * entry because per-operation locking releases after each insert.
		 */
		index_rel = try_index_open(
				local_state->shared->index_oid, RowExclusiveLock);
		if (index_rel == NULL)
			continue;

		tp_acquire_index_lock(local_state, LW_EXCLUSIVE);

		/* Unified spill path. */
		(void)tp_do_spill(local_state, index_rel, NULL);

		tp_release_index_lock(local_state);
		index_close(index_rel, RowExclusiveLock);
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
