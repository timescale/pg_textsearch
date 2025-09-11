/*-------------------------------------------------------------------------
 *
 * stringtable.h
 *	  Custom hash table implementation for variable-length string interning
 *	  Handles small strings (words/terms) with table-level locking
 *
 * IDENTIFICATION
 *	  src/stringtable.h
 *
 *-------------------------------------------------------------------------
 */
#pragma once

#include "postgres.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"

/*
 * Forward declarations
 */
typedef struct TpStringHashEntry TpStringHashEntry;
typedef struct TpPostingList TpPostingList;
typedef struct TpPostingEntry TpPostingEntry;

/*
 * Individual hash entry in DSA
 * Uses dsa_pointer for all references within the DSA
 */
struct TpStringHashEntry
{
	dsa_pointer	next_dp;		/* DSA pointer to next entry in chain (InvalidDsaPointer = end) */
	dsa_pointer	string_dp;		/* DSA pointer to string */
	uint32		string_length;	/* Length of string */
	uint32		hash_value;		/* Cached hash value */

	/* Application data - string interning */
	dsa_pointer	posting_list_dp; /* DSA pointer to TpPostingList (InvalidDsaPointer = none) */
	int32		doc_freq;		/* Document frequency */
};

/*
 * Custom hash table structure in DSA
 * Stores variable-length strings (typically words/terms from text search)
 */
typedef struct TpStringHashTable
{
	/* Table metadata */
	uint32		bucket_count;	/* Number of hash buckets (power of 2) */
	uint32		entry_count;	/* Total entries in table */
	uint32		collision_count; /* Statistics for monitoring */

	/* Memory management - no limits enforced for now */
	uint32		max_entries;	/* Reserved for future use */
	
	/* Hash buckets - array of dsa_pointer to first entry (InvalidDsaPointer = empty) */
	dsa_pointer	buckets_dp;		/* DSA pointer to array of dsa_pointer (bucket array) */
	
	/* Note: With DSA, we don't need separate pools - everything is allocated via dsa_allocate */
	/* The DSA area handle is stored in the containing TpIndexState structure */
}			TpStringHashTable;

/*
 * Transaction-local lock state
 * Not stored in shared memory - per-backend state
 */
typedef struct TpTransactionLockState
{
	bool		has_lock;		/* True if we have acquired the lock */
	LWLockMode	lock_mode;		/* LW_SHARED or LW_EXCLUSIVE */
	bool		callback_registered; /* True if xact callback registered */
}			TpTransactionLockState;

/* No global hash table anymore - each index has its own in DSA */

/* Hash table creation and initialization with DSA */
extern TpStringHashTable *tp_hash_table_create_dsa(dsa_area *area, uint32 initial_buckets);
extern void tp_hash_table_init_dsa(TpStringHashTable *ht, dsa_area *area, uint32 initial_buckets);

/* Core hash table operations with DSA */
extern TpStringHashEntry *tp_hash_lookup_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len);
extern TpStringHashEntry *tp_hash_insert_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len);
extern bool tp_hash_delete_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len);
extern bool tp_hash_resize_dsa(dsa_area *area, TpStringHashTable *ht, uint32 new_bucket_count);
extern void tp_hash_table_clear_dsa(dsa_area *area, TpStringHashTable *ht);

/* Statistics and debugging */
extern void tp_hash_stats_dsa(dsa_area *area, TpStringHashTable *ht, uint32 *buckets_used, 
							  double *avg_chain_length, uint32 *max_chain_length);

/* Posting list management in DSA */
extern TpPostingList *tp_alloc_posting_list_dsa(dsa_area *area);
extern TpPostingEntry *tp_alloc_posting_entries_dsa(dsa_area *area, uint32 capacity);
extern TpPostingEntry *tp_realloc_posting_entries_dsa(dsa_area *area, TpPostingList *posting_list, uint32 new_capacity);

/* Helper functions for dsa_pointer conversion */
extern TpPostingList *tp_get_posting_list_from_dp(dsa_area *area, dsa_pointer dp);
extern TpPostingEntry *tp_get_posting_entries_from_dp(dsa_area *area, dsa_pointer dp);
extern char *tp_get_string_from_dp(dsa_area *area, dsa_pointer dp);

/* Default sizing constants */
#define TP_HASH_DEFAULT_BUCKETS		1024	/* Initial bucket count */
#define TP_HASH_MAX_LOAD_FACTOR		0.75	/* Rehash when load > this */
/* Note: No need for fixed pool sizes with DSA - memory is allocated dynamically */

