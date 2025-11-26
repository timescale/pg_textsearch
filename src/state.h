/*-------------------------------------------------------------------------
 *
 * state.h
 *	  Index state management structures and functions
 *
 * This header defines TpSharedIndexState and TpLocalIndexState structures
 * for managing Tapir index state across backends.
 *
 * IDENTIFICATION
 *	  src/state.h
 *
 *-------------------------------------------------------------------------
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
	int32				total_terms;		/* Total unique terms interned */

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
	int32  total_docs; /* Total number of documents */
	int64  total_len;  /* Total length of all documents */
	float8 idf_sum;	   /* Sum of all IDF values for average IDF calculation */
	/* Note: num_unique_terms is available as memtable->total_terms */

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
} TpLocalIndexState;

/* Function declarations for index state management */
extern TpLocalIndexState *tp_get_local_index_state(Oid index_oid);
extern TpLocalIndexState *tp_get_local_index_state_if_cached(Oid index_oid);
extern void tp_release_local_index_state(TpLocalIndexState *local_state);
extern TpLocalIndexState			 *
tp_create_shared_index_state(Oid index_oid, Oid heap_oid);
extern void tp_cleanup_index_shared_memory(Oid index_oid);
extern void tp_destroy_shared_index_state(TpSharedIndexState *shared_state);
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
