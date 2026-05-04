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
	dshash_table_handle string_hash_handle; /* Handle to dshash string
											 * table */
	pg_atomic_uint64 total_postings;		/* Total posting entries */

	/* Document length hash table in DSA */
	dshash_table_handle doc_lengths_handle; /* Handle for document
											 * length hash table */

	/* Counters for memory estimation (soft limit) */
	pg_atomic_uint64 num_terms;		 /* Unique terms in string table */
	pg_atomic_uint64 total_term_len; /* Sum of all term string lengths */
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
	pg_atomic_uint32 total_docs; /* Total number of documents */
	pg_atomic_uint64 total_len;	 /* Total length of all documents */

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
	 * Generation counter for docid page cache invalidation.
	 * Incremented under LW_EXCLUSIVE when docid pages are
	 * cleared (spill). Backends compare their cached
	 * generation to detect stale docid page caches.
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
 * registry, and return a local-state handle. If start_locked is true
 * the per-index LWLock is acquired EXCLUSIVE before the registry
 * entry becomes visible — used by tp_rebuild_index_from_disk to keep
 * the standby's startup process from racing redo against the docid
 * scan. The caller is then responsible for releasing the lock once
 * the rebuild is done.
 */
extern TpLocalIndexState *
tp_create_shared_index_state(Oid index_oid, Oid heap_oid, bool start_locked);
extern TpLocalIndexState			 *
tp_create_build_index_state(Oid index_oid, Oid heap_oid);
extern void tp_cleanup_index_shared_memory(Oid index_oid);
extern void tp_recreate_build_dsa(TpLocalIndexState *local_state);
extern void tp_finalize_build_mode(TpLocalIndexState *local_state);
extern void tp_cleanup_build_mode_on_abort(void);
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

/* Subtransaction cleanup */
extern void tp_cleanup_subxact_abort(SubTransactionId mySubid);
extern void tp_promote_subxact_states(
		SubTransactionId mySubid, SubTransactionId parentSubid);
