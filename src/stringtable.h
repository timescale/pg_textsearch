/*-------------------------------------------------------------------------
 *
 * stringtable.h
 *	  String interning hash table using PostgreSQL's dshash
 *	  Handles small strings (words/terms) with built-in concurrency
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
#include "lib/dshash.h"

/*
 * Forward declarations
 */
typedef struct TpStringHashEntry TpStringHashEntry;
typedef struct TpPostingList TpPostingList;
typedef struct TpPostingEntry TpPostingEntry;

/*
 * dshash entry structure
 * Key is the string pointer itself (fixed size)
 * Hash and comparison functions dereference the pointer to compare actual string content
 */
struct TpStringHashEntry
{
	/* Key part - must be first in struct for dshash */
	dsa_pointer	string_dp;		/* DSA pointer to string (this is our fixed-size key) */
	
	/* Value part - application data */
	uint32		string_length;	/* Length of string */
	uint32		hash_value;		/* Cached hash value for performance */
	dsa_pointer	posting_list_dp; /* DSA pointer to TpPostingList (InvalidDsaPointer = none) */
	int32		doc_freq;		/* Document frequency */
};

/*
 * String table wrapper around dshash
 * Maintains the same interface as before but uses dshash internally
 */
typedef struct TpStringHashTable
{
	dshash_table *dshash;		/* The underlying dshash table */
	dshash_table_handle handle;	/* Handle for sharing across processes */
	
	/* Statistics - maintained for compatibility */
	uint32		entry_count;	/* Total entries in table (approx) */
	uint32		max_entries;	/* Reserved for future use */
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

/* Each index has its own string table backed by dshash */

/* Hash table creation and initialization */
extern TpStringHashTable *tp_hash_table_create_dsa(dsa_area *area, uint32 initial_buckets);
extern TpStringHashTable *tp_hash_table_attach_dsa(dsa_area *area, dshash_table_handle handle);
extern void tp_hash_table_detach_dsa(TpStringHashTable *ht);
extern void tp_hash_table_destroy_dsa(TpStringHashTable *ht);

/* Core hash table operations */
extern TpStringHashEntry *tp_hash_lookup_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len);
extern TpStringHashEntry *tp_hash_insert_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len);
extern bool tp_hash_delete_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len);
extern void tp_hash_table_clear_dsa(dsa_area *area, TpStringHashTable *ht);

/* Handle management for cross-process sharing */
extern dshash_table_handle tp_hash_table_get_handle(TpStringHashTable *ht);

/* Posting list management in DSA */
extern TpPostingList *tp_alloc_posting_list_dsa(dsa_area *area);
extern TpPostingEntry *tp_alloc_posting_entries_dsa(dsa_area *area, uint32 capacity);
extern TpPostingEntry *tp_realloc_posting_entries_dsa(dsa_area *area, TpPostingList *posting_list, uint32 new_capacity);

/* Helper functions for dsa_pointer conversion */
extern TpPostingList *tp_get_posting_list_from_dp(dsa_area *area, dsa_pointer dp);
extern TpPostingEntry *tp_get_posting_entries_from_dp(dsa_area *area, dsa_pointer dp);
extern char *tp_get_string_from_dp(dsa_area *area, dsa_pointer dp);

/* Default sizing constants */
#define TP_HASH_DEFAULT_BUCKETS		1024	/* Initial bucket count (ignored with dshash) */

/* LWLock tranche for string table locking */
#define TP_STRING_HASH_TRANCHE_ID	LWTRANCHE_FIRST_USER_DEFINED

