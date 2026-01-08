/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * state.h - Index state management structures
 *
 * LOCKING DISCIPLINE
 * ==================
 *
 * This extension uses a two-level locking scheme:
 *
 * 1. Per-Index LWLock (TpSharedIndexState.lock)
 *    - One LWLock per index, stored in the shared state
 *    - Acquired once per transaction via tp_acquire_index_lock()
 *    - Released at transaction end via tp_release_index_lock()
 *    - Writers: LW_EXCLUSIVE mode for INSERT/UPDATE/DELETE
 *    - Readers: LW_SHARED mode for SELECT queries
 *
 * 2. dshash Internal Locks (partition-level)
 *    - Held briefly during hash table operations
 *    - Released immediately after lookup/insert completes
 *
 * IMPORTANT: The per-index LWLock MUST be held when:
 *   - Accessing memtable data structures
 *   - Reading posting list entries returned by string table lookup
 *   - Modifying corpus statistics (total_docs, total_len)
 *
 * The string table lookup functions (tp_string_table_lookup, etc.) release
 * their dshash locks before returning, relying on the per-index LWLock to
 * prevent concurrent destruction. Callers MUST ensure the per-index lock
 * is held before calling these functions.
 *
 * Lock ordering (to prevent deadlocks):
 *   1. Per-index LWLock (acquired first, held for transaction duration)
 *   2. dshash partition locks (acquired/released during operations)
 *   3. Buffer locks (for metapage/segment access)
 *
 * WARNING: Do not upgrade from LW_SHARED to LW_EXCLUSIVE while holding
 * the lock. This can deadlock if another backend also holds LW_SHARED.
 * Instead, release and re-acquire (with potential for stale data).
 */
#pragma once

#include <postgres.h>

#include <lib/dshash.h>
#include <storage/lwlock.h>
#include <utils/dsa.h>

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
 * Memtable structure - encapsulates the inverted index
 * Contains the string interning table and document length tracking
 */
typedef struct TpMemtable
{
	/* String interning hash table in DSA */
	dshash_table_handle string_hash_handle; /* Handle to dshash string table */
	int64				total_postings;		/* Total posting entries for spill
											 * threshold */

	/* Document length hash table in DSA */
	dshash_table_handle doc_lengths_handle; /* Handle for document length hash
											 * table */
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

	/* Corpus statistics for BM25 scoring */
	int32 total_docs; /* Total number of documents */
	int64 total_len;  /* Total length of all documents */

	/*
	 * Per-index LWLock for transaction-level serialization.
	 * Writers acquire this in exclusive mode once per transaction.
	 * Readers acquire this in shared mode once per transaction.
	 * This ensures memory consistency on NUMA systems and proper
	 * transaction isolation.
	 */
	LWLock lock; /* Transaction-level lock for this index */
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

	/* Transaction-level lock tracking */
	bool	   lock_held; /* True if we hold the lock in this transaction */
	LWLockMode lock_mode; /* Mode we're holding (LW_SHARED or LW_EXCLUSIVE) */

	/* Bulk load tracking: terms added in current transaction */
	int64 terms_added_this_xact;
} TpLocalIndexState;

/* Function declarations for index state management */
extern TpLocalIndexState *tp_get_local_index_state(Oid index_oid);
extern TpLocalIndexState			 *
tp_create_shared_index_state(Oid index_oid, Oid heap_oid);
extern void tp_cleanup_index_shared_memory(Oid index_oid);
extern TpLocalIndexState *tp_rebuild_index_from_disk(Oid index_oid);
extern void				  tp_rebuild_posting_lists_from_docids(
					  Relation			 index_rel,
					  TpLocalIndexState *local_state,
					  TpIndexMetaPage	 metap);

/* Helper function for accessing memtable from local state */
extern TpMemtable *get_memtable(TpLocalIndexState *local_state);

/* Transaction-level lock management */
extern void
tp_acquire_index_lock(TpLocalIndexState *local_state, LWLockMode mode);
extern void tp_release_index_lock(TpLocalIndexState *local_state);
extern void tp_release_all_index_locks(void);

/* Memtable management */
extern void tp_clear_memtable(TpLocalIndexState *local_state);

/* Bulk load auto-spill */
extern void tp_bulk_load_spill_check(void);
extern void tp_reset_bulk_load_counters(void);
