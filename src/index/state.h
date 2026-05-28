/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * state.h - Index state management structures
 */
#pragma once

#include <postgres.h>

#include <lib/dshash.h>
#include <port/atomics.h>
#include <storage/lwlock.h>
#include <utils/dsa.h>

/*
 * BlockNumber / InvalidBlockNumber are used by the cache's apply
 * cursor below (and are not transitively pulled in by the other
 * Postgres headers we include here).
 */
#include <storage/block.h>

/* Forward declarations */
struct TpMemtable;
typedef struct TpIndexMetaPageData *TpIndexMetaPage;
typedef struct RelationData		   *Relation;

/*
 * Header of the DSM segment for each index
 * Contains metadata and space for the DSA area
 */
typedef struct TpDsmSegmentHeader
{
	dsm_handle	dsm_handle;		 /* DSM segment handle for recovery */
	dsa_pointer shared_state_dp; /* DSA pointer to TpSharedIndexState */
	/* DSA area space follows immediately after this header */
} TpDsmSegmentHeader;

/*
 * In-memory memtable cache (see docs/memtable_cache.md).
 *
 * The cache is derived state layered on top of the on-disk memtable
 * page chain (which remains the source of truth, per #374).  This
 * struct holds the lifecycle handles, apply cursor, and footprint
 * accounting; the apply protocol, cold build, read source, spill
 * consumption, and memory cap live in cache.c / cache_source.c.
 *
 * Lifetimes:
 *   - string_hash_handle / doc_lengths_handle:
 *       DSHASH_HANDLE_INVALID until cold_build allocates them; reset
 *       back to DSHASH_HANDLE_INVALID by tp_cache_clear.
 *   - apply_lock / lock:
 *       initialized once at TpMemtable allocation, never destroyed
 *       across cache clears (the structs survive; tp_cache_clear only
 *       drops the dshash tables and resets the cursor).
 *   - cursor_*:
 *       (0, InvalidBlockNumber, 0, 0) at init and after tp_cache_clear.
 *       Mutated only under apply_lock EXCL by reader catchup,
 *       cold lazy build, and spill catchup.
 *   - estimated_bytes:
 *       0 at init.  Maintained by the apply paths under apply_lock;
 *       read lock-free by the 3-tier memory cap.
 *
 * Lock order (see docs/memtable_cache.md "Lock order"):
 *   per-index LWLock (in TpSharedIndexState) → apply_lock → lock
 *     → dshash buckets → TpPostingList.lock
 */
typedef struct TpMemtable
{
	/* dshash inverted index: term -> TpStringHashEntry. */
	dshash_table_handle string_hash_handle;

	/* dshash doc-length map: ctid -> TpDocLengthEntry. */
	dshash_table_handle doc_lengths_handle;

	/*
	 * Serializes every path that mutates the cache (reader catchup,
	 * cold build, spill catchup, tp_cache_clear).  Acquired AFTER the
	 * per-index LWLock.  EXCL only.
	 */
	LWLock apply_lock;

	/*
	 * Lifetime lock for served TpDataSources.  SHARED for the lifetime
	 * of a source (open through close); EXCL only for full drop /
	 * evict.  apply_lock holders may take SHARED here concurrently
	 * with served readers.
	 */
	LWLock lock;

	/*
	 * Apply cursor.  Identifies the next chain record the cache
	 * expects to consume.  Mutated only under apply_lock EXCL by
	 * reader catchup, cold lazy build, and spill catchup.
	 *
	 * cursor_gen_spill_count is the generation token captured from
	 * TpSharedIndexState.spill_generation at cold-build time; a
	 * mismatch on a later apply means a spill raced through and the
	 * cache is stale (see docs/memtable_cache.md "Spill detection").
	 *
	 * (cursor_next_blkno, cursor_next_off) is the logical position of
	 * the next chain record to apply; InvalidBlockNumber means "no
	 * cache yet, cold_build is required before any catchup can
	 * advance the cursor".
	 *
	 * cursor_seq is a monotonic apply counter for cheap "did anything
	 * change" checks; read lock-free.
	 */
	uint64			 cursor_gen_spill_count;
	BlockNumber		 cursor_next_blkno;
	uint16			 cursor_next_off;
	pg_atomic_uint64 cursor_seq;

	/*
	 * Cache footprint, updated by apply paths under apply_lock;
	 * read lock-free for the 3-tier memory cap.
	 */
	pg_atomic_uint64 estimated_bytes;
} TpMemtable;

/*
 * Shared index state - stored in DSA
 * This structure is shared across all backends and contains only
 * data that can be safely stored in dynamic shared memory.
 * All pointers must be dsa_pointer type.
 */
typedef struct TpSharedIndexState
{
	/* Index identification */
	Oid index_oid; /* OID of this index */
	Oid heap_oid;  /* OID of the indexed heap relation */

	/* Memtable stored in DSA */
	dsa_pointer memtable_dp; /* DSA pointer to TpMemtable */

	/*
	 * True while this shared state is published by a backend that
	 * is still in BUILD mode (CREATE INDEX).  In that mode
	 * `memtable_dp` is a pointer into the **private** DSA of the
	 * building backend and MUST NOT be dereferenced through the
	 * global DSA by any other backend (notably the cross-index
	 * eviction walker at src/memtable/cache.c:evict_walk_cb).
	 *
	 * Set to true at registration time in
	 * tp_create_build_index_state(), cleared in
	 * tp_finalize_build_mode() under
	 * tp_registry_eviction_mutex EXCL (the same mutex evict_largest
	 * walks under) so the walker observes a consistent
	 * (is_build_mode=false, memtable_dp in global DSA) transition.
	 *
	 * Read lock-free by the eviction walker; correctness comes from
	 * the publish-before-register barrier on the true→true path
	 * and the eviction_mutex on the true→false transition.
	 */
	bool is_build_mode;

	/*
	 * Auto-spill heuristic for the on-disk memtable: number of
	 * chain pages currently published in the page chain.
	 * Incremented after each successful page-publish
	 * GenericXLogFinish in src/memtable/log.c; reset to 0 after
	 * tp_spill_finalize() completes.  Not WAL-logged: on crash
	 * recovery, the counter starts at 0 even if the chain
	 * survived.  Worst case the heuristic overshoots by ~one
	 * threshold's worth of pages between restart and the next
	 * normal merge — acceptable since the counter only governs
	 * when to spill, not correctness.
	 */
	pg_atomic_uint32 chain_page_count;

	/*
	 * Cached estimated memtable size in bytes, updated
	 * atomically by writers. Used to maintain the global
	 * estimated_total_bytes counter without scanning.
	 */
	pg_atomic_uint64 estimated_bytes;

	/*
	 * Per-index LWLock for transaction-level serialization.
	 * Writers acquire this in exclusive mode once per transaction.
	 * Readers acquire this in shared mode once per transaction.
	 * This ensures memory consistency on NUMA systems and proper
	 * transaction isolation.
	 */
	LWLock lock; /* Per-index lock for this index */

	/*
	 * Spill generation counter.  Bumped by tp_spill_finalize()
	 * under LW_EXCLUSIVE after the on-disk chain is truncated.
	 * Acts as the in-memory memtable cache's invalidation
	 * token: a cache built against generation N becomes stale
	 * the moment this counter advances to N+1, even if a new
	 * chain page happens to be allocated at the same block
	 * number as the old head (ABA hole on head_blkno alone).
	 * See docs/memtable_cache.md ("Spill detection").
	 *
	 * Not WAL-logged: standby query paths don't use the cache
	 * (RecoveryInProgress() disables it), so primary-only
	 * generation tracking is sufficient.
	 */
	pg_atomic_uint64 spill_generation;
} TpSharedIndexState;

/*
 * Local index state - backend-specific
 * This structure is private to each backend and contains the DSA
 * attachment and other backend-specific data.
 */
typedef struct TpLocalIndexState
{
	/* Pointer to shared state in registry */
	TpSharedIndexState *shared;

	/* DSA attachment for this backend */
	dsa_area *dsa; /* Attached DSA area for this index */

	/*
	 * Build mode flag: If true, this backend owns a private DSA that
	 * gets destroyed and recreated on each spill for perfect memory
	 * reclamation. If false, uses shared DSA for concurrent access.
	 */
	bool is_build_mode;

	/* Transaction-level lock tracking */
	bool	   lock_held; /* True if we hold the lock in this transaction */
	LWLockMode lock_mode; /* Mode we're holding (LW_SHARED or LW_EXCLUSIVE) */

	/* Bulk load tracking: terms added in current transaction */
	int64 terms_added_this_xact;

	/* Amortization counter for global soft limit check */
	int docs_since_global_check;

	/*
	 * Subtransaction tracking: which subtransaction created this
	 * state. Used to clean up on subtransaction abort (e.g.,
	 * ROLLBACK TO SAVEPOINT after CREATE INDEX).
	 * InvalidSubTransactionId means not created in any tracked
	 * subtransaction (e.g., rebuilt from disk on restart).
	 */
	SubTransactionId created_in_subxact;
} TpLocalIndexState;

/* Function declarations for index state management */
extern TpLocalIndexState *tp_get_local_index_state(Oid index_oid);
/*
 * Allocate the per-index shared state, register it in the global
 * registry, and return a local-state handle.
 *
 * If `reuse_if_exists` is true and an entry for `index_oid` is
 * already registered when we go to insert, the just-allocated
 * DSA is freed and we attach to the existing entry — this is
 * what the cold-start bootstrap path uses, so concurrent
 * rebuilds from two backends don't unregister each other.
 *
 * If false, an existing entry is unregistered and replaced; the
 * parallel-build completion path uses this on the assumption
 * that any pre-existing entry is stale (left by a crashed
 * earlier CREATE INDEX).
 */
extern TpLocalIndexState *tp_create_shared_index_state(
		Oid index_oid, Oid heap_oid, bool reuse_if_exists);
extern TpLocalIndexState			 *
tp_create_build_index_state(Oid index_oid, Oid heap_oid);
extern void tp_cleanup_index_shared_memory(Oid index_oid);
extern void tp_finalize_build_mode(TpLocalIndexState *local_state);
extern void tp_cleanup_build_mode_on_abort(void);
extern TpLocalIndexState *tp_rebuild_index_from_disk(Oid index_oid);

/* Helper function for accessing memtable from local state */
extern TpMemtable *get_memtable(TpLocalIndexState *local_state);

/* Transaction-level lock management */
extern void
tp_acquire_index_lock(TpLocalIndexState *local_state, LWLockMode mode);
extern void tp_release_index_lock(TpLocalIndexState *local_state);
extern void tp_release_all_index_locks(void);

/* Bulk load auto-spill */
extern void tp_bulk_load_spill_check(void);
extern void tp_reset_bulk_load_counters(void);

/* Subtransaction cleanup */
extern void tp_cleanup_subxact_abort(SubTransactionId mySubid);
extern void tp_promote_subxact_states(
		SubTransactionId mySubid, SubTransactionId parentSubid);
