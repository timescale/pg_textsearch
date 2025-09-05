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
#ifndef TP_STRINGTABLE_H
#define TP_STRINGTABLE_H

#include "postgres.h"
#include "storage/lwlock.h"

/*
 * Forward declaration for TpStringHashEntry
 */
typedef struct TpStringHashEntry TpStringHashEntry;

/*
 * Individual hash entry in shared memory
 * Uses offsets instead of pointers for shared memory compatibility
 */
struct TpStringHashEntry
{
	uint32		next_offset;	/* Offset to next entry in chain (0 = end) */
	uint32		string_offset;	/* Offset to string in pool */
	uint32		string_length;	/* Length of string */
	uint32		hash_value;		/* Cached hash value */

	/* Application data - string interning */
	void	   *posting_list;	/* Pointer to TpPostingList (not in shared memory) */
	int32		doc_freq;		/* Document frequency */
};

/*
 * Custom hash table structure in shared memory
 * Stores variable-length strings (typically words/terms from text search)
 */
typedef struct TpStringHashTable
{
	/* Table metadata */
	uint32		bucket_count;	/* Number of hash buckets (power of 2) */
	uint32		entry_count;	/* Total entries in table */
	uint32		collision_count; /* Statistics for monitoring */

	/* Memory management */
	uint32		max_entries;	/* Maximum entries allowed (memory limit) */
	char	   *string_pool;	/* Base of string storage pool */
	Size		pool_size;		/* Total pool size */
	Size		pool_used;		/* Bytes used so far */
	
	/* Entry pool management */
	TpStringHashEntry *entry_pool; /* Base of entry storage pool */
	uint32		entry_pool_size; /* Maximum entries in pool */

	/* Hash buckets - array of offsets to first entry (0 = empty) */
	uint32	   *buckets;		/* Offset to TpStringHashEntry chain */
	
	/* Per-table locking */
	LWLock	   *lock;			/* Per-table lock for this string table */
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

/* Global variables */
extern TpStringHashTable *tp_string_hash_table;
extern TpTransactionLockState tp_xact_lock_state;

/* Hash table creation and initialization */
extern Size tp_hash_table_shmem_size(uint32 max_entries, Size string_pool_size);
extern TpStringHashTable *tp_hash_table_create(uint32 initial_buckets, Size string_pool_size, uint32 max_entries);
extern void tp_hash_table_init(TpStringHashTable *ht, uint32 initial_buckets, Size string_pool_size, uint32 max_entries);

/* Core hash table operations */
extern TpStringHashEntry *tp_hash_lookup(TpStringHashTable *ht, const char *str, size_t len);
extern TpStringHashEntry *tp_hash_insert(TpStringHashTable *ht, const char *str, size_t len);
extern bool tp_hash_delete(TpStringHashTable *ht, const char *str, size_t len);
extern bool tp_hash_resize(TpStringHashTable *ht, uint32 new_bucket_count);

/* Transaction-level locking */
extern void tp_acquire_string_table_lock(TpStringHashTable *string_table, bool for_write);
extern void tp_release_string_table_lock(TpStringHashTable *string_table);

/* Statistics and debugging */
extern void tp_hash_stats(TpStringHashTable *ht, uint32 *buckets_used, 
						  double *avg_chain_length, uint32 *max_chain_length);

/* Memory budget calculation functions */
extern uint32 tp_calculate_max_hash_entries(void);
extern uint32 tp_calculate_max_posting_entries(void);



/* Default sizing constants */
#define TP_HASH_DEFAULT_BUCKETS		1024	/* Initial bucket count */
#define TP_HASH_MAX_LOAD_FACTOR		0.75	/* Rehash when load > this */
#define TP_HASH_DEFAULT_STRING_POOL_SIZE (32 * 1024 * 1024) /* 32MB */

#endif							/* TP_STRINGTABLE_H */