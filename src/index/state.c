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
#include "index/state.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "replication/replication.h"
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
					index_rel, entry->local_state, TP_MIN_SPILL_POSTINGS);
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
 * backend has touched, so the docid chain doesn't have to be
 * replayed from heap on the next server start.  Skipped on clean
 * client exit (code == 0); fires on any FATAL (cluster shutdown,
 * pg_terminate_backend, etc.).
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
 * Create a new shared index state and return local state
 *
 * This is called during CREATE INDEX to set up the initial shared state
 * and return a ready-to-use local state to avoid double DSA attachment.
 */
TpLocalIndexState *
tp_create_shared_index_state(Oid index_oid, Oid heap_oid)
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
	pg_atomic_init_u32(&shared_state->total_docs, 0);
	pg_atomic_init_u64(&shared_state->total_len, 0);
	pg_atomic_init_u64(&shared_state->estimated_bytes, 0);

	/*
	 * Initialize the per-index LWLock using a fixed tranche ID.
	 * Using a fixed ID avoids exhausting tranche IDs when creating many
	 * indexes (e.g., partitioned tables with 500+ partitions).
	 */
	LWLockInitialize(&shared_state->lock, TP_TRANCHE_INDEX_LOCK);
	pg_atomic_init_u64(&shared_state->spill_generation, 0);

	/* Allocate and initialize memtable */
	memtable_dp = dsa_allocate(dsa, sizeof(TpMemtable));
	if (!DsaPointerIsValid(memtable_dp))
		elog(ERROR, "Failed to allocate memtable in DSA");

	memtable = (TpMemtable *)dsa_get_address(dsa, memtable_dp);
	memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	pg_atomic_init_u64(&memtable->total_postings, 0);
	pg_atomic_init_u64(&memtable->num_terms, 0);
	pg_atomic_init_u64(&memtable->total_term_len, 0);
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;

	shared_state->memtable_dp = memtable_dp;

	/* Check if index is already registered (rebuild case) */
	if (tp_registry_lookup(index_oid) != NULL)
	{
		/* This is a rebuild (e.g., VACUUM FULL) - unregister old index first
		 */
		tp_registry_unregister(index_oid);
	}

	/* Register in global registry */
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
	pg_atomic_init_u32(&shared_state->total_docs, 0);
	pg_atomic_init_u64(&shared_state->total_len, 0);
	shared_state->memtable_dp =
			InvalidDsaPointer; /* Memtable in private DSA */
	pg_atomic_init_u64(&shared_state->estimated_bytes, 0);

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
	pg_atomic_init_u64(&memtable->total_postings, 0);
	pg_atomic_init_u64(&memtable->num_terms, 0);
	pg_atomic_init_u64(&memtable->total_term_len, 0);
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;

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
 * Recreate private DSA for build mode
 *
 * This is called after spilling to disk. We destroy the entire private DSA
 * (freeing ALL memory to OS) and create a fresh one for the next batch.
 * This provides perfect memory reclamation.
 */
void
tp_recreate_build_dsa(TpLocalIndexState *local_state)
{
	dsa_area   *new_dsa;
	TpMemtable *new_memtable;
	dsa_pointer memtable_dp;

	Assert(local_state != NULL);
	Assert(local_state->is_build_mode);

	/*
	 * Detach from old DSA. This calls dsa_detach() which, for a
	 * non-attached DSA (no other backends), completely destroys it
	 * and returns ALL memory to the OS. Perfect reclamation!
	 */
	if (local_state->dsa)
		dsa_detach(local_state->dsa);

	/*
	 * Create fresh private DSA using fixed tranche ID.
	 * Using a fixed ID avoids exhausting tranche IDs when creating many
	 * indexes (e.g., partitioned tables with 500+ partitions).
	 */
	new_dsa = dsa_create(TP_TRANCHE_BUILD_DSA);
	if (!new_dsa)
		elog(ERROR, "Failed to recreate private DSA for build");

	/* Allocate fresh memtable in new DSA */
	memtable_dp = dsa_allocate(new_dsa, sizeof(TpMemtable));
	if (!DsaPointerIsValid(memtable_dp))
		elog(ERROR, "Failed to allocate memtable in new private DSA");

	new_memtable = (TpMemtable *)dsa_get_address(new_dsa, memtable_dp);
	new_memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	pg_atomic_init_u64(&new_memtable->total_postings, 0);
	pg_atomic_init_u64(&new_memtable->num_terms, 0);
	pg_atomic_init_u64(&new_memtable->total_term_len, 0);
	new_memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;

	/* Update shared state with new memtable pointer */
	local_state->shared->memtable_dp = memtable_dp;

	/* Update local state with new DSA */
	local_state->dsa = new_dsa;
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

	Assert(local_state != NULL);
	Assert(local_state->is_build_mode);

	/*
	 * Destroy the private DSA. This returns ALL memory to the OS.
	 * After build, the memtable should be empty (all data spilled to disk).
	 */
	if (local_state->dsa)
	{
		dsa_detach(local_state->dsa);
		local_state->dsa = NULL;
	}

	/*
	 * Attach to the global shared DSA for runtime operation.
	 * This is the same DSA used by all backends for concurrent access.
	 */
	global_dsa = tp_registry_get_dsa();
	if (!global_dsa)
		elog(ERROR, "Failed to get global DSA for runtime mode");

	local_state->dsa = global_dsa;

	/*
	 * Allocate a fresh memtable in the global DSA.
	 * This memtable will be shared with other backends.
	 */
	memtable_dp = dsa_allocate(global_dsa, sizeof(TpMemtable));
	if (!DsaPointerIsValid(memtable_dp))
		elog(ERROR, "Failed to allocate memtable in global DSA");

	memtable = (TpMemtable *)dsa_get_address(global_dsa, memtable_dp);
	memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	pg_atomic_init_u64(&memtable->total_postings, 0);
	pg_atomic_init_u64(&memtable->num_terms, 0);
	pg_atomic_init_u64(&memtable->total_term_len, 0);
	memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;

	/* Update shared state with new memtable pointer */
	local_state->shared->memtable_dp = memtable_dp;

	/* Transition to runtime mode */
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
		 */
		if (local_state->shared != NULL)
		{
			Oid			index_oid = local_state->shared->index_oid;
			dsa_pointer shared_dp = tp_registry_lookup_dsa(index_oid);

			/* Subtract estimate before freeing shared state */
			tp_subtract_index_estimate(local_state->shared);

			if (DsaPointerIsValid(shared_dp) && global_dsa != NULL)
			{
				/* Free shared state from global DSA */
				dsa_free(global_dsa, shared_dp);
			}

			/* Unregister from registry */
			tp_registry_unregister(index_oid);

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
		else if (ls->shared != NULL && global_dsa != NULL)
		{
			/*
			 * Runtime mode: memtable is in global DSA.
			 * Destroy hash tables before freeing to avoid
			 * leaking DSA memory.
			 */
			if (DsaPointerIsValid(ls->shared->memtable_dp))
			{
				TpMemtable *mt = (TpMemtable *)
						dsa_get_address(global_dsa, ls->shared->memtable_dp);

				if (mt->string_hash_handle != DSHASH_HANDLE_INVALID)
				{
					dshash_table *ht = tp_string_table_attach(
							global_dsa, mt->string_hash_handle);
					if (ht)
					{
						tp_string_table_clear(global_dsa, ht);
						dshash_destroy(ht);
					}
				}

				if (mt->doc_lengths_handle != DSHASH_HANDLE_INVALID)
				{
					dshash_table *ht = tp_doclength_table_attach(
							global_dsa, mt->doc_lengths_handle);
					if (ht)
						dshash_destroy(ht);
				}

				dsa_free(global_dsa, ls->shared->memtable_dp);
			}
		}

		/* Free shared state from global DSA and unregister */
		if (ls->shared != NULL)
		{
			Oid			index_oid = ls->shared->index_oid;
			dsa_pointer shared_dp = tp_registry_lookup_dsa(index_oid);

			tp_subtract_index_estimate(ls->shared);

			if (DsaPointerIsValid(shared_dp) && global_dsa != NULL)
				dsa_free(global_dsa, shared_dp);

			tp_registry_unregister(index_oid);
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
	TpMemtable			 *memtable;
	LocalStateCacheEntry *entry = NULL;
	bool				  found;

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

	/* Get shared state to access memtable */
	shared_state = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);
	memtable = (TpMemtable *)dsa_get_address(dsa, shared_state->memtable_dp);

	/* Clear and destroy the string hash table if it exists */
	if (memtable->string_hash_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_table *string_hash;

		string_hash =
				tp_string_table_attach(dsa, memtable->string_hash_handle);
		if (string_hash != NULL)
		{
			/* Free all strings and posting lists */
			tp_string_table_clear(dsa, string_hash);
			dshash_destroy(string_hash);
		}
	}

	/* Destroy the document lengths hash table if it exists */
	if (memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_table *doc_lengths_hash;

		doc_lengths_hash =
				tp_doclength_table_attach(dsa, memtable->doc_lengths_handle);
		if (doc_lengths_hash != NULL)
			dshash_destroy(doc_lengths_hash);
	}

	/* Subtract estimate from global counter before freeing */
	tp_subtract_index_estimate(shared_state);

	/* Free shared state structures from DSA */
	dsa_free(dsa, shared_state->memtable_dp);

	/* Free shared_state */
	dsa_free(dsa, shared_dp);

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

	/* Unregister from global registry AFTER cleanup */
	tp_registry_unregister(index_oid);
}

/*
 * Rebuild index state from disk after PostgreSQL restart
 * This recreates the DSA area and shared state from docid pages.
 *
 * On a hot standby this can race the startup process: redo of an
 * INSERT_TERMS record looks up the registry, and once we register
 * the new shared state, redo would otherwise start writing into the
 * same memtable we are still rebuilding from docid pages — and
 * because docid-page WAL is ordered after INSERT_TERMS WAL for any
 * given insert, redo and the rebuild would both add the same doc
 * (duplicate posting entries, inflated total_docs). To prevent
 * that, we ask tp_create_shared_index_state to register the entry
 * with the per-index LWLock already held EXCLUSIVE; the standby's
 * single-threaded WAL replay simply blocks on that lock until the
 * docid walk finishes and we release it, then proceeds with the
 * normal redo path against the now-populated memtable.
 */
TpLocalIndexState *
tp_rebuild_index_from_disk(Oid index_oid)
{
	Relation		   index_rel;
	TpIndexMetaPage	   metap;
	TpLocalIndexState *local_state;
	Relation		   heap_rel;
	BlockNumber		   heap_blocks;

	/* Open the index relation */
	index_rel = index_open(index_oid, AccessShareLock);
	if (index_rel == NULL)
	{
		elog(WARNING, "Could not open index %u for recovery", index_oid);
		return NULL;
	}

	/* Get metapage */
	metap = tp_get_metapage(index_rel);
	if (metap == NULL)
	{
		index_close(index_rel, AccessShareLock);
		elog(WARNING, "Could not read metapage for index %u", index_oid);
		return NULL;
	}

	/* Validate that this is actually our metapage and not stale data */
	if (metap->magic != TP_METAPAGE_MAGIC)
	{
		uint32 found_magic = metap->magic;

		index_close(index_rel, AccessShareLock);
		pfree(metap);
		elog(WARNING,
			 "Invalid magic number in metapage for index %u: "
			 "expected 0x%08X, found 0x%08X",
			 index_oid,
			 TP_METAPAGE_MAGIC,
			 found_magic);
		return NULL;
	}

	/*
	 * Additional validation: Check if the heap relation has been truncated
	 * or recreated since the index was built. If the heap is empty but the
	 * metapage shows documents, this is stale data.
	 *
	 * This check is necessary to handle cases where:
	 * - The table was TRUNCATEd but the index wasn't properly cleaned
	 * - The index is out of sync due to an incomplete operation
	 * - Data corruption occurred
	 * Without this check, we could attempt to scan non-existent heap tuples.
	 */
	heap_rel = relation_open(index_rel->rd_index->indrelid, AccessShareLock);
	heap_blocks = RelationGetNumberOfBlocks(heap_rel);
	relation_close(heap_rel, AccessShareLock);

	if (heap_blocks == 0 && metap->total_docs > 0)
	{
		Oid heap_oid = index_rel->rd_index->indrelid;

		/* Heap is empty but metapage shows documents - stale data */
		elog(WARNING,
			 "Index %u metapage shows %lu documents but heap is "
			 "empty - ignoring stale data",
			 index_oid,
			 (unsigned long)metap->total_docs);
		index_close(index_rel, AccessShareLock);
		pfree(metap);

		/* Create fresh shared state for the index */
		return tp_create_shared_index_state(index_oid, heap_oid);
	}

	/* Check if there's actually anything to recover */
	if (metap->total_docs == 0 &&
		metap->first_docid_page == InvalidBlockNumber &&
		metap->level_heads[0] == InvalidBlockNumber)
	{
		/* Empty index - nothing to recover */
		index_close(index_rel, AccessShareLock);
		pfree(metap);

		/* Still create the shared state for the empty index */
		return tp_create_shared_index_state(
				index_oid, index_rel->rd_index->indrelid);
	}

	local_state = tp_create_shared_index_state(
			index_oid, index_rel->rd_index->indrelid);

	if (local_state != NULL)
	{
		/*
		 * Drain WAL replay before walking docid pages.
		 *
		 * On a hot standby, the docid pages we are about to scan
		 * are kept in sync with the primary's docid-page WAL by
		 * the startup process. WAL ordering is INSERT_TERMS first,
		 * then docid-page update — but with two concurrent writers
		 * the records can interleave on disk, so a docid-page
		 * update for doc D can land *after* the INSERT_TERMS for
		 * an earlier doc D'. If we registered the shared state and
		 * walked immediately, we could observe a docid page that
		 * does not yet contain D' (its docid-page WAL hasn't
		 * replayed) while the INSERT_TERMS for D' has either
		 * already replayed (skipped — registry was empty at the
		 * time) or is about to replay against our partially-built
		 * memtable. Either way D' would be visible on disk shortly
		 * after the walk finishes but missing from the memtable.
		 *
		 * Waiting for replay to catch up to the receive LSN gives
		 * us a snapshot in which every docid-page update for any
		 * INSERT_TERMS that has been emitted is visible on disk.
		 * Combined with the idempotency gate in
		 * tp_add_document_terms, concurrent replay during the walk
		 * can no longer cause double-add or miss.
		 */
		if (RecoveryInProgress())
		{
			XLogRecPtr target = GetWalRcvFlushRecPtr(NULL, NULL);
			if (target == InvalidXLogRecPtr)
				target = GetXLogReplayRecPtr(NULL);
			while (GetXLogReplayRecPtr(NULL) < target)
			{
				CHECK_FOR_INTERRUPTS();
				pg_usleep(1000L);
			}
		}

		tp_acquire_index_lock(local_state, LW_SHARED);
		tp_rebuild_posting_lists_from_docids(index_rel, local_state, metap);

		/*
		 * Load corpus statistics from metapage. This is needed for indexes
		 * built with parallel workers (which write directly to segments
		 * without docid pages) or if docid recovery didn't fully restore
		 * the stats. The metapage is the authoritative source for total_docs
		 * and total_len.
		 */
		pg_atomic_write_u32(
				&local_state->shared->total_docs, metap->total_docs);
		pg_atomic_write_u64(&local_state->shared->total_len, metap->total_len);

		tp_release_index_lock(local_state);
	}

	/* Clean up */
	if (metap)
		pfree(metap);
	index_close(index_rel, AccessShareLock);

	return local_state;
}

/*
 * Rebuild posting lists from docid pages stored on disk
 * This scans the docid pages, retrieves documents from heap, and rebuilds the
 * posting lists
 */
void
tp_rebuild_posting_lists_from_docids(
		Relation		   index_rel,
		TpLocalIndexState *local_state,
		TpIndexMetaPage	   metap)
{
	Buffer			   docid_buf;
	Page			   docid_page;
	TpDocidPageHeader *docid_header;
	ItemPointer		   docids;
	BlockNumber		   current_page;
	BlockNumber		   last_valid_page = InvalidBlockNumber;
	Relation		   heap_rel;
	IndexInfo		  *indexInfo;
	EState			  *estate;
	ExprContext		  *econtext;
	TupleTableSlot	  *eval_slot;
	Datum			   idx_values[INDEX_MAX_KEYS];
	bool			   idx_isnull[INDEX_MAX_KEYS];
	bool			   chain_truncated = false;

	if (!metap || metap->first_docid_page == InvalidBlockNumber)
	{
		return;
	}

	/* Inform user that recovery is starting */
	elog(INFO,
		 "Recovering pg_textsearch index %u from disk",
		 index_rel->rd_id);

	/* Open the heap relation to fetch document text */
	heap_rel = relation_open(index_rel->rd_index->indrelid, AccessShareLock);

	/*
	 * Ensure the string hash table is initialized before
	 * rebuilding posting lists. Recovery is single-threaded
	 * so no lock is needed, but
	 * tp_get_or_create_posting_list requires the table to
	 * exist when not in build mode.
	 */
	tp_ensure_string_table_initialized(local_state);

	/* Set up expression evaluation */
	indexInfo = BuildIndexInfo(index_rel);
	estate	  = CreateExecutorState();
	econtext  = GetPerTupleExprContext(estate);
	eval_slot = MakeSingleTupleTableSlot(
			RelationGetDescr(heap_rel), &TTSOpsBufferHeapTuple);

	if (indexInfo->ii_Predicate != NIL)
		indexInfo->ii_PredicateState =
				ExecPrepareQual(indexInfo->ii_Predicate, estate);

	current_page = metap->first_docid_page;

	/* Scan through all docid pages */
	while (current_page != InvalidBlockNumber)
	{
		/* Read the docid page */
		docid_buf = ReadBuffer(index_rel, current_page);
		LockBuffer(docid_buf, BUFFER_LOCK_SHARE);
		docid_page	 = BufferGetPage(docid_buf);
		docid_header = (TpDocidPageHeader *)PageGetContents(docid_page);

		/*
		 * Validate this is actually a docid page and not stale data.
		 */
		if (docid_header->magic != TP_DOCID_PAGE_MAGIC)
		{
			uint32 found_magic = docid_header->magic;

			UnlockReleaseBuffer(docid_buf);

			if (found_magic == 0)
			{
				/*
				 * All-zero page: a crash occurred after the chain
				 * pointer was flushed but before this page reached
				 * disk. Treat as end-of-chain and recover what we
				 * have.
				 */
				elog(WARNING,
					 "Unflushed docid page at block %u "
					 "(all zeros) - truncating recovery "
					 "chain",
					 current_page);
			}
			else
			{
				/*
				 * Non-zero but wrong magic: actual on-disk
				 * corruption. Don't silently continue.
				 */
				elog(ERROR,
					 "Corrupted docid page at block %u: "
					 "expected magic 0x%08X, found "
					 "0x%08X",
					 current_page,
					 TP_DOCID_PAGE_MAGIC,
					 found_magic);
			}
			chain_truncated = true;
			break;
		}

		last_valid_page = current_page;

		/* Get docids array with proper alignment */
		docids = (ItemPointer)((char *)docid_header +
							   MAXALIGN(sizeof(TpDocidPageHeader)));

		/* Process each docid on this page */
		for (unsigned int i = 0; i < docid_header->num_docids; i++)
		{
			ItemPointer	  ctid = &docids[i];
			HeapTupleData tuple_data;
			HeapTuple	  tuple = &tuple_data;
			Buffer		  heap_buf;
			bool		  valid;
			BlockNumber	  heap_blkno;

			/* Validate the ItemPointer before attempting fetch */
			if (!ItemPointerIsValid(ctid))
			{
				elog(WARNING,
					 "Invalid ItemPointer in docid page"
					 " - skipping");
				continue;
			}

			/* Initialize tuple for heap_fetch */
			tuple->t_self = *ctid;

			/*
			 * Try to fetch the document from heap using the
			 * stored ctid. We use heap_fetch with SnapshotAny
			 * to see all tuples, but we need to be careful
			 * because the tuple might not exist if this is
			 * stale data from a previous index incarnation.
			 *
			 * First check if the block exists in the heap.
			 */
			heap_blkno = ItemPointerGetBlockNumber(ctid);
			if (heap_blkno >= RelationGetNumberOfBlocks(heap_rel))
			{
				/*
				 * Block doesn't exist in heap - stale data.
				 * This can occur when:
				 * - The heap was VACUUMed and pages truncated
				 * - The table was TRUNCATEd after index built
				 * - Crash between index update and heap update
				 * - Data corruption resulted in invalid ctids
				 * Skip these entries rather than failing.
				 */
				continue;
			}

			/* Fetch document from heap */
			valid = heap_fetch(heap_rel, SnapshotAny, tuple, &heap_buf, true);
			if (!valid || !HeapTupleIsValid(tuple))
			{
				if (heap_buf != InvalidBuffer)
					ReleaseBuffer(heap_buf);
				continue; /* Skip invalid documents */
			}

			/* Evaluate index expression */
			ExecStoreBufferHeapTuple(tuple, eval_slot, heap_buf);
			econtext->ecxt_scantuple = eval_slot;
			FormIndexDatum(
					indexInfo, eval_slot, estate, idx_values, idx_isnull);

			/* Check partial index predicate */
			if (indexInfo->ii_Predicate != NIL &&
				!ExecQual(indexInfo->ii_PredicateState, econtext))
			{
				ExecClearTuple(eval_slot);
				ResetExprContext(econtext);
				ReleaseBuffer(heap_buf);
				continue;
			}

			if (!idx_isnull[0])
			{
				text *document_text = DatumGetTextPP(idx_values[0]);
				int32 doc_length;

				/* tp_add_document_terms already increments the atomic */
				(void)tp_process_document_text(
						document_text,
						ctid,
						metap->text_config_oid,
						local_state,
						NULL,
						&doc_length);
			}

			ExecClearTuple(eval_slot);
			ResetExprContext(econtext);
			ReleaseBuffer(heap_buf);
		}

		/* Move to next page */
		current_page = docid_header->next_page;

		UnlockReleaseBuffer(docid_buf);
	}

	ExecDropSingleTupleTableSlot(eval_slot);
	FreeExecutorState(estate);
	relation_close(heap_rel, AccessShareLock);

	/*
	 * If we truncated the chain due to an invalid page, fix the
	 * chain so the corrupted tail is unreachable. We overwrite
	 * next_page on the last valid page rather than clearing the
	 * entire chain, so that a second crash before the memtable
	 * is spilled doesn't lose the valid docid pages.
	 */
	if (chain_truncated && last_valid_page != InvalidBlockNumber)
	{
		Buffer			   trunc_buf;
		GenericXLogState  *xlog_state;
		Page			   trunc_page;
		TpDocidPageHeader *trunc_hdr;

		trunc_buf = ReadBuffer(index_rel, last_valid_page);
		LockBuffer(trunc_buf, BUFFER_LOCK_EXCLUSIVE);

		xlog_state = GenericXLogStart(index_rel);
		trunc_page = GenericXLogRegisterBuffer(xlog_state, trunc_buf, 0);
		trunc_hdr  = (TpDocidPageHeader *)PageGetContents(trunc_page);
		trunc_hdr->next_page = InvalidBlockNumber;
		GenericXLogFinish(xlog_state);

		UnlockReleaseBuffer(trunc_buf);
	}
	else if (chain_truncated)
	{
		/*
		 * The very first page was invalid — no valid pages
		 * at all. Clear the metapage pointer.
		 */
		tp_clear_docid_pages(index_rel);
	}

	/* Log recovery completion */
	if (local_state && local_state->shared)
	{
		elog(INFO,
			 "Recovery complete for pg_textsearch index %u: "
			 "%u documents restored",
			 index_rel->rd_id,
			 pg_atomic_read_u32(&local_state->shared->total_docs));

		/*
		 * Reset terms_added_this_xact to prevent bulk load spill from
		 * triggering after recovery. Recovery re-adds all documents to the
		 * memtable, which would otherwise count toward the bulk load
		 * threshold and incorrectly trigger a segment write.
		 */
		local_state->terms_added_this_xact	 = 0;
		local_state->docs_since_global_check = 0;
	}
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
 * Clear the memtable after segment spill by destroying hash tables completely.
 *
 * This destroys the string hash table and document lengths table entirely,
 * allowing their DSA memory to be freed. The tables will be recreated on
 * demand when new documents are added. This aggressive approach ensures
 * that DSA segments can be released back to the OS.
 *
 * Corpus statistics are preserved as they represent the overall index state.
 */
void
tp_clear_memtable(TpLocalIndexState *local_state)
{
	TpMemtable *memtable;

	if (!local_state || !local_state->shared)
		return;

	memtable = get_memtable(local_state);
	if (!memtable)
		return;

	/* Subtract this index's estimate from the global counter */
	tp_subtract_index_estimate(local_state->shared);

	/*
	 * BUILD MODE: Destroy entire private DSA and create fresh one.
	 * This provides perfect memory reclamation - ALL memory returns to OS.
	 */
	if (local_state->is_build_mode)
	{
		/* Destroy and recreate private DSA */
		tp_recreate_build_dsa(local_state);
		return;
	}

	/*
	 * RUNTIME MODE: Use best-effort reclamation with dshash_destroy +
	 * dsa_trim. This is the existing behavior for concurrent inserts.
	 */
	{
		dshash_table *string_table;
		dshash_table *doc_lengths_table;
		Size		  dsa_size_before;

		dsa_size_before = dsa_get_total_size(local_state->dsa);

		/* Destroy the string hash table */
		if (memtable->string_hash_handle != DSHASH_HANDLE_INVALID)
		{
			string_table = tp_string_table_attach(
					local_state->dsa, memtable->string_hash_handle);
			if (string_table)
			{
				tp_string_table_clear(local_state->dsa, string_table);
				dshash_destroy(string_table);
			}
			memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
		}

		/* Destroy the document lengths hash table */
		if (memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
		{
			doc_lengths_table = tp_doclength_table_attach(
					local_state->dsa, memtable->doc_lengths_handle);
			if (doc_lengths_table)
				dshash_destroy(doc_lengths_table);
			memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;
		}

		/* Reset counters */
		pg_atomic_write_u64(&memtable->total_postings, 0);
		pg_atomic_write_u64(&memtable->num_terms, 0);
		pg_atomic_write_u64(&memtable->total_term_len, 0);

		/* Try to reclaim DSA memory (best effort) */
		dsa_trim(local_state->dsa);

		(void)dsa_size_before;
	}
}

/*
 * Check if any index should spill to disk due to bulk load threshold.
 * Spill is triggered when terms added this transaction exceeds threshold.
 *
 * Note: memtable_spill_threshold is now checked in real-time via
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
		BlockNumber		   segment_root;
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

		/* Write the segment */
		segment_root = tp_write_segment(local_state, index_rel);

		/* Clear memtable and update metapage if spill succeeded */
		if (segment_root != InvalidBlockNumber)
		{
			tp_clear_memtable(local_state);
			tp_clear_docid_pages(index_rel);
			tp_sync_metapage_stats(index_rel, local_state);

			if (RelationNeedsWAL(index_rel))
			{
				START_CRIT_SECTION();
				tp_xlog_spill(index_rel, segment_root);
				END_CRIT_SECTION();
			}
			else
			{
				tp_link_l0_chain_head(index_rel, segment_root);
			}

			tp_maybe_compact_level(index_rel, 0);
		}

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
