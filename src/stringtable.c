/*-------------------------------------------------------------------------
 *
 * stringtable.c
 *	  Custom hash table implementation for variable-length string interning
 *
 * This implements a simple hash table for string interning with
 * table-level locking. Memory layout is designed for shared memory usage.
 * Typically handles small strings (words/terms from text search processing).
 *
 * IDENTIFICATION
 *	  src/stringtable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "stringtable.h"
#include "memtable.h"
#include "posting.h"
#include "constants.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "access/xact.h"
#include "utils/memutils.h"
#include "common/hashfn_unstable.h"

/* Global variables */
TpStringHashTable *tp_string_hash_table = NULL;
TpTransactionLockState tp_xact_lock_state = {false, 0, false};


/* Static function declarations */
static void tp_hash_maybe_resize(TpStringHashTable *ht);
static uint32 tp_next_power_of_2(uint32 n);
static bool tp_hash_validate_entry_offset(TpStringHashTable *ht, uint32 entry_offset);


/*
 * Calculate required shared memory size for hash table
 */
Size
tp_hash_table_shmem_size(uint32 max_entries, Size string_pool_size)
{
	Size		size = 0;
	uint32		bucket_count;

	/* Calculate bucket count based on max entries and load factor */
	bucket_count = tp_next_power_of_2((uint32) (max_entries / TP_HASH_MAX_LOAD_FACTOR));

	/* Hash table header */
	size = add_size(size, sizeof(TpStringHashTable));

	/* Buckets array */
	size = add_size(size, mul_size(bucket_count, sizeof(uint32)));

	/* Entry pool (estimate based on max entries) */
	size = add_size(size, mul_size(max_entries, sizeof(TpStringHashEntry)));

	/* String pool */
	size = add_size(size, string_pool_size);

	return size;
}

/*
 * Create and initialize hash table in shared memory
 */
TpStringHashTable *
tp_hash_table_create(uint32 initial_buckets, Size string_pool_size, uint32 max_entries)
{
	Size		total_size;
	char	   *base_ptr;
	TpStringHashTable *ht;

	/* Calculate total memory needed */
	total_size = tp_hash_table_shmem_size(max_entries, string_pool_size);

	/* Allocate from shared memory */
	base_ptr = (char *) ShmemAlloc(total_size);
	if (!base_ptr)
		elog(ERROR, "Could not allocate %zu bytes for hash table", total_size);

	/* Initialize the hash table */
	ht = (TpStringHashTable *) base_ptr;

	/* Zero out the entire allocated memory for sanitizer safety */
	memset(base_ptr, 0, total_size);

	tp_hash_table_init(ht, initial_buckets, string_pool_size, max_entries);

	return ht;
}

/*
 * Initialize hash table structure (used by both create and attach)
 */
void
tp_hash_table_init(TpStringHashTable *ht, uint32 initial_buckets, Size string_pool_size, uint32 max_entries)
{
	Size		buckets_size;
	Size		entry_pool_size;
	char	   *ptr;
	uint32		i;

	/* Ensure bucket count is power of 2 */
	initial_buckets = tp_next_power_of_2(initial_buckets);

	/* Initialize header */
	ht->bucket_count = initial_buckets;
	ht->entry_count = 0;
	ht->collision_count = 0;
	ht->max_entries = max_entries;
	ht->pool_size = string_pool_size;
	ht->pool_used = 0;

	/* Calculate layout - leave space for entries between buckets and string pool */
	buckets_size = initial_buckets * sizeof(uint32);
	entry_pool_size = (initial_buckets * TP_HASH_MAX_LOAD_FACTOR) * sizeof(TpStringHashEntry);
	ptr = (char *) ht + sizeof(TpStringHashTable);

	/* Set up buckets array */
	ht->buckets = (uint32 *) ptr;
	ptr += buckets_size;

	/* Set up entry pool */
	ht->entry_pool = (TpStringHashEntry *) ptr;
	ht->entry_pool_size = (uint32)(initial_buckets * TP_HASH_MAX_LOAD_FACTOR);
	ptr += entry_pool_size;

	/* Set up string pool after the entry pool */
	ht->string_pool = ptr;

	/* Initialize buckets to empty (offset 0) */
	for (i = 0; i < initial_buckets; i++)
		ht->buckets[i] = 0;

	elog(DEBUG1, "Tapir hash table initialized: %u buckets, %zu string pool",
		 initial_buckets, string_pool_size);
}

/*
 * Look up a string in the hash table
 * Returns NULL if not found
 */
TpStringHashEntry *
tp_hash_lookup(TpStringHashTable *ht, const char *str, size_t len)
{
	uint32		hash;
	uint32		bucket;
	uint32		entry_offset;
	TpStringHashEntry *entry;
	const char *entry_str;

	if (!ht || !str)
		return NULL;

	hash = fasthash32(str, len, 0);
	bucket = hash & (ht->bucket_count - 1);
	entry_offset = ht->buckets[bucket];

	/* Walk the chain for this bucket */
	while (entry_offset != 0)
	{
		/* Validate entry offset before dereferencing */
		if (!tp_hash_validate_entry_offset(ht, entry_offset))
		{
			elog(WARNING, "Invalid entry offset %u detected in hash table lookup", entry_offset);
			break;
		}

		entry = (TpStringHashEntry *) ((char *) ht + entry_offset);

		/* Quick hash comparison first */
		if (entry->hash_value == hash && entry->string_length == len)
		{
			/* Validate string offset before dereferencing */
			if (entry->string_offset >= ht->pool_size)
			{
				elog(WARNING, "Invalid string offset %u detected in hash table entry", entry->string_offset);
				break;
			}

			/* Hash matches, compare actual string */
			entry_str = ht->string_pool + entry->string_offset;
			if (memcmp(str, entry_str, len) == 0)
				return entry;
		}

		entry_offset = entry->next_offset;
	}

	return NULL;
}

/*
 * Insert a string into the hash table
 * Returns pointer to the new entry
 * Assumes caller has exclusive lock
 */
TpStringHashEntry *
tp_hash_insert(TpStringHashTable *ht, const char *str, size_t len)
{
	uint32		hash;
	uint32		bucket;
	TpStringHashEntry *entry;
	uint32		entry_offset;
	char	   *entry_str;

	if (!ht || !str || len == 0)
		elog(ERROR, "Invalid parameters to tp_hash_insert");

	/* Check if we've reached maximum entry capacity */
	if (ht->entry_count >= ht->max_entries)
		elog(ERROR, "Hash table capacity exceeded: %u entries, limit %u (memory budget per index: %dMB)",
			 ht->entry_count, ht->max_entries, tp_shared_memory_size);

	/* Check if we have enough space (including null terminator) */
	if (ht->pool_used + len + 1 > ht->pool_size)
		elog(ERROR, "String pool exhausted: need %zu bytes, have %zu available",
			 len + 1, ht->pool_size - ht->pool_used);

	/* Check load factor and resize if needed */
	tp_hash_maybe_resize(ht);

	hash = fasthash32(str, len, 0);
	bucket = hash & (ht->bucket_count - 1);

	/* Check bounds - ensure we don't overflow entry pool */
	if (ht->entry_count >= ht->entry_pool_size)
		elog(ERROR, "Hash table entry pool exhausted: %u entries, max %u",
			 ht->entry_count, ht->entry_pool_size);

	/* Allocate the next available entry from the entry pool */
	entry = &ht->entry_pool[ht->entry_count];
	entry_offset = (char *) entry - (char *) ht;

	/* Allocate space for the string (including null terminator) */
	entry_str = ht->string_pool + ht->pool_used;
	memcpy(entry_str, str, len);
	entry_str[len] = '\0';  /* Ensure null termination */

	/* Initialize the entry - zero out first for sanitizer safety */
	memset(entry, 0, sizeof(TpStringHashEntry));

	entry->next_offset = ht->buckets[bucket];  /* Insert at head of chain */
	entry->string_offset = ht->pool_used;
	entry->string_length = len;
	entry->hash_value = hash;
	entry->posting_list = NULL;  /* Explicitly set to NULL for sanitizer safety */
	entry->doc_freq = 0;

	/* Update hash table state */
	ht->buckets[bucket] = entry_offset;
	ht->pool_used += len + 1;  /* Include null terminator in pool usage */
	ht->entry_count++;

	/* Track collisions for statistics */
	if (entry->next_offset != 0)
		ht->collision_count++;

	return entry;
}

/*
 * Delete a string from the hash table
 * Returns true if found and deleted, false if not found
 */
bool
tp_hash_delete(TpStringHashTable *ht, const char *str, size_t len)
{
	uint32		hash;
	uint32		bucket;
	uint32		entry_offset;
	uint32		prev_offset = 0;
	TpStringHashEntry *entry;
	const char *entry_str;

	if (!ht || !str)
		return false;

	hash = fasthash32(str, len, 0);
	bucket = hash & (ht->bucket_count - 1);
	entry_offset = ht->buckets[bucket];

	/* Walk the chain looking for the entry */
	while (entry_offset != 0)
	{
		/* Validate entry offset before dereferencing */
		if (!tp_hash_validate_entry_offset(ht, entry_offset))
		{
			elog(WARNING, "Invalid entry offset %u detected in hash table delete", entry_offset);
			break;
		}

		entry = (TpStringHashEntry *) ((char *) ht + entry_offset);

		if (entry->hash_value == hash && entry->string_length == len)
		{
			/* Validate string offset before dereferencing */
			if (entry->string_offset >= ht->pool_size)
			{
				elog(WARNING, "Invalid string offset %u detected in hash table delete", entry->string_offset);
				break;
			}

			entry_str = ht->string_pool + entry->string_offset;
			if (memcmp(str, entry_str, len) == 0)
			{
				/* Found it - remove from chain */
				if (prev_offset == 0)
				{
					/* First in chain */
					ht->buckets[bucket] = entry->next_offset;
				}
				else
				{
					/* Middle/end of chain */
					TpStringHashEntry *prev_entry;

					if (!tp_hash_validate_entry_offset(ht, prev_offset))
					{
						elog(WARNING, "Invalid prev entry offset %u in hash table delete", prev_offset);
						return false;
					}
					prev_entry = (TpStringHashEntry *) ((char *) ht + prev_offset);
					prev_entry->next_offset = entry->next_offset;
				}

				ht->entry_count--;
				return true;
			}
		}

		prev_offset = entry_offset;
		entry_offset = entry->next_offset;
	}

	return false;
}

/*
 * Transaction-level lock management
 */
void
tp_acquire_string_table_lock(TpStringHashTable *string_table, bool for_write)
{
	LWLockMode	mode;

	if (tp_xact_lock_state.has_lock)
	{
		/* Already have a lock */
		if (for_write && tp_xact_lock_state.lock_mode == LW_SHARED)
		{
			/* Need to upgrade from shared to exclusive */
			LWLockRelease(string_table->lock);
			LWLockAcquire(string_table->lock, LW_EXCLUSIVE);
			tp_xact_lock_state.lock_mode = LW_EXCLUSIVE;
		}
		return;
	}

	/* First lock acquisition in this transaction */
	mode = for_write ? LW_EXCLUSIVE : LW_SHARED;

	LWLockAcquire(string_table->lock, mode);
	tp_xact_lock_state.has_lock = true;
	tp_xact_lock_state.lock_mode = mode;

	/* Note: Per-index locks are released explicitly, no transaction callback needed */
}

void
tp_release_string_table_lock(TpStringHashTable *string_table)
{
	if (tp_xact_lock_state.has_lock)
	{
		LWLockRelease(string_table->lock);
		tp_xact_lock_state.has_lock = false;
		tp_xact_lock_state.lock_mode = 0;
	}
}

/* Transaction callback removed - per-index locks managed directly */

/*
 * Get hash table statistics
 */
void
tp_hash_stats(TpStringHashTable *ht, uint32 *buckets_used,
			  double *avg_chain_length, uint32 *max_chain_length)
{
	uint32		used = 0;
	uint32		total_chain_length = 0;
	uint32		max_chain = 0;
	uint32		i;

	for (i = 0; i < ht->bucket_count; i++)
	{
		if (ht->buckets[i] != 0)
		{
			uint32		chain_length = 0;
			uint32		entry_offset = ht->buckets[i];

			used++;

			/* Count entries in this chain */
			while (entry_offset != 0)
			{
				TpStringHashEntry *entry;

				/* Validate entry offset before dereferencing */
				if (!tp_hash_validate_entry_offset(ht, entry_offset))
				{
					elog(WARNING, "Invalid entry offset %u detected in hash table stats", entry_offset);
					break;
				}

				entry = (TpStringHashEntry *) ((char *) ht + entry_offset);
				chain_length++;
				entry_offset = entry->next_offset;
			}

			total_chain_length += chain_length;
			if (chain_length > max_chain)
				max_chain = chain_length;
		}
	}

	*buckets_used = used;
	*avg_chain_length = used > 0 ? (double) total_chain_length / used : 0.0;
	*max_chain_length = max_chain;
}

/*
 * Helper function: maybe resize hash table if load factor too high
 */
static void
tp_hash_maybe_resize(TpStringHashTable *ht)
{
	double		load_factor = (double) ht->entry_count / ht->bucket_count;
	uint32		new_bucket_count;

	if (load_factor > TP_HASH_MAX_LOAD_FACTOR)
	{
		/* Double the bucket count */
		new_bucket_count = ht->bucket_count * 2;

		/* Only resize if we haven't exceeded reasonable limits */
		if (new_bucket_count <= 32768) /* Reasonable limit for shared memory */
		{
			if (tp_hash_resize(ht, new_bucket_count))
			{
				elog(DEBUG1, "Hash table resized from %u to %u buckets (load factor was %.2f)",
					 ht->bucket_count / 2, ht->bucket_count, load_factor);
			}
			else
			{
				elog(WARNING, "Hash table resize failed, continuing with high load factor %.2f",
					 load_factor);
			}
		}
		else
		{
			elog(WARNING, "Hash table load factor %.2f exceeds %.2f but cannot resize beyond %u buckets",
				 load_factor, TP_HASH_MAX_LOAD_FACTOR, new_bucket_count / 2);
		}
	}
}

/*
 * Helper function: find next power of 2 >= n
 */
static uint32
tp_next_power_of_2(uint32 n)
{
	uint32		result = 1;

	if (n == 0)
		return 1;

	while (result < n)
		result <<= 1;

	return result;
}

/*
 * Validate entry offset is within reasonable bounds
 * Helps detect memory corruption and prevent crashes
 */
static bool
tp_hash_validate_entry_offset(TpStringHashTable *ht, uint32 entry_offset)
{
	char *entry_pool_start;
	char *entry_pool_end;

	if (entry_offset == 0)
		return true; /* 0 = end of chain is valid */

	/* Calculate entry pool bounds */
	entry_pool_start = (char *) ht->entry_pool;
	entry_pool_end = entry_pool_start + (ht->entry_pool_size * sizeof(TpStringHashEntry));

	/* Entry offset should point to a valid location within the entry pool */
	if (entry_offset < (entry_pool_start - (char *) ht))
		return false;

	if (entry_offset >= (entry_pool_end - (char *) ht))
		return false;

	return true;
}

/*
 * Resize hash table to new bucket count
 * This is complex in shared memory since we can't reallocate
 * Returns true on success, false on failure
 *
 * MEMORY SAFETY NOTES:
 * - All entry offsets are validated before dereferencing
 * - Memory moves are done with proper bounds checking
 * - Temporary storage avoids overlap issues
 */
bool
tp_hash_resize(TpStringHashTable *ht, uint32 new_bucket_count)
{
	uint32		old_bucket_count;
	uint32		i;
	Size		new_buckets_size;
	Size		old_buckets_size;
	Size		available_space;
	Size		size_diff;
	char	   *target_buckets;
	char	   *base_ptr;
	Size		total_hash_size;

	if (!ht || new_bucket_count <= ht->bucket_count)
		return false;

	/* Ensure new bucket count is power of 2 */
	new_bucket_count = tp_next_power_of_2(new_bucket_count);

	old_bucket_count = ht->bucket_count;
	new_buckets_size = new_bucket_count * sizeof(uint32);
	old_buckets_size = old_bucket_count * sizeof(uint32);
	size_diff = new_buckets_size - old_buckets_size;

	/* Check if we have enough total space for larger bucket array */
	available_space = ht->pool_size - ht->pool_used;
	if (size_diff > available_space)
	{
		elog(DEBUG1, "Not enough space to resize hash table: need %zu more bytes, have %zu",
			 size_diff, available_space);
		return false;
	}

	/* Calculate memory layout carefully */
	base_ptr = (char *) ht;
	target_buckets = base_ptr + sizeof(TpStringHashTable);
	total_hash_size = sizeof(TpStringHashTable) + new_buckets_size +
					  (ht->entry_count * sizeof(TpStringHashEntry));

	/* Ensure we don't exceed reasonable memory bounds */
	if (total_hash_size > ht->pool_size / 2)
	{
		elog(DEBUG1, "Hash table would become too large: %zu bytes", total_hash_size);
		return false;
	}

	/* First, adjust the string pool and entry pool if needed */
	if (size_diff > 0)
	{
		/* Move string pool contents to make room for larger bucket array */
		if (ht->pool_used > 0)
		{
			char *old_pool_start = ht->string_pool;
			char *new_pool_start = old_pool_start + size_diff;

			/* Ensure we're not overwriting anything */
			if (new_pool_start + ht->pool_used > base_ptr + ht->pool_size)
			{
				elog(DEBUG1, "String pool would overflow after resize");
				return false;
			}

			memmove(new_pool_start, old_pool_start, ht->pool_used);
		}

		/* Update string pool pointer */
		ht->string_pool += size_diff;
	}

	/* Update entry pool pointer (always recalculate based on new layout) */
	ht->entry_pool = (TpStringHashEntry *) (target_buckets + new_buckets_size);

	/* Update hash table metadata */
	ht->bucket_count = new_bucket_count;
	ht->buckets = (uint32 *) target_buckets;
	ht->entry_pool_size = (uint32)(new_bucket_count * TP_HASH_MAX_LOAD_FACTOR);

	/* Initialize all new buckets to empty */
	for (i = 0; i < new_bucket_count; i++)
		ht->buckets[i] = 0;

	/* Now rehash all existing entries into the new bucket array */
	for (i = 0; i < ht->entry_count; i++)
	{
		TpStringHashEntry *entry = &ht->entry_pool[i];
		uint32 entry_offset = (char *) entry - base_ptr;
		uint32 new_bucket;

		if (entry->string_length == 0)
			continue;

		new_bucket = entry->hash_value & (new_bucket_count - 1);

		/* Insert at head of new bucket chain */
		entry->next_offset = ht->buckets[new_bucket];
		ht->buckets[new_bucket] = entry_offset;
	}

	elog(DEBUG2, "Hash table successfully resized from %u to %u buckets",
		 old_bucket_count, new_bucket_count);

	return true;
}

/*
 * Calculate maximum hash table entries based on memory budget
 */
uint32
tp_calculate_max_hash_entries(void)
{
	Size		string_table_memory;
	Size		per_entry_cost;
	
	/* Calculate memory allocated to string table (25% of per-index budget) */
	string_table_memory = (Size) tp_shared_memory_size * 1024 * 1024 * TP_STRING_TABLE_MEMORY_FRACTION;
	
	/* Cost per entry: hash entry + average string length */
	per_entry_cost = sizeof(TpStringHashEntry) + (TP_AVG_TERM_LENGTH + 1); /* +1 for null terminator */
	
	return (uint32) (string_table_memory / per_entry_cost);
}

/*
 * Calculate maximum posting list entries based on memory budget
 */
uint32
tp_calculate_max_posting_entries(void)
{
	Size		posting_memory;
	Size		per_entry_cost;
	
	/* Calculate memory allocated to posting lists (75% of per-index budget) */
	posting_memory = (Size) tp_shared_memory_size * 1024 * 1024 * TP_POSTING_LISTS_MEMORY_FRACTION;
	
	/* Cost per posting entry */
	per_entry_cost = sizeof(TpPostingEntry);
	
	return (uint32) (posting_memory / per_entry_cost);
}
