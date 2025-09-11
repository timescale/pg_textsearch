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
#include "common/hashfn.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "access/xact.h"
#include "utils/memutils.h"
#if PG_VERSION_NUM >= 170000
#include "common/hashfn_unstable.h"
#else
#include "common/hashfn.h"
#endif

/* Static function declarations */
/*
 * ========================================================================
 * DSA-BASED STRING TABLE FUNCTIONS
 * ========================================================================
 */

/*
 * Create and initialize a new hash table in DSA memory
 */
TpStringHashTable *
tp_hash_table_create_dsa(dsa_area *area, uint32 initial_buckets)
{
	TpStringHashTable *ht;
	dsa_pointer *buckets;
	dsa_pointer ht_dp, buckets_dp;
	uint32 i;
	
	Assert(area != NULL);
	Assert(initial_buckets > 0);
	
	/* Ensure bucket count is power of 2 */
	if ((initial_buckets & (initial_buckets - 1)) != 0)
	{
		/* Round up to next power of 2 */
		initial_buckets--;
		initial_buckets |= initial_buckets >> 1;
		initial_buckets |= initial_buckets >> 2;
		initial_buckets |= initial_buckets >> 4;
		initial_buckets |= initial_buckets >> 8;
		initial_buckets |= initial_buckets >> 16;
		initial_buckets++;
	}
	
	/* Allocate main hash table structure */
	ht_dp = dsa_allocate(area, sizeof(TpStringHashTable));
	ht = dsa_get_address(area, ht_dp);
	
	/* Allocate bucket array */
	buckets_dp = dsa_allocate(area, initial_buckets * sizeof(dsa_pointer));
	buckets = dsa_get_address(area, buckets_dp);
	
	/* Initialize hash table */
	ht->bucket_count = initial_buckets;
	ht->entry_count = 0;
	ht->collision_count = 0;
	ht->max_entries = 0; /* Not enforced for now */
	ht->buckets_dp = buckets_dp;
	
	/* Initialize all buckets to empty */
	for (i = 0; i < initial_buckets; i++)
		buckets[i] = InvalidDsaPointer;
		
	elog(DEBUG2, "Created DSA string table: %u buckets", initial_buckets);
	return ht;
}

/*
 * Initialize an existing hash table structure in DSA
 * Used when attaching to existing DSA segment
 */
void
tp_hash_table_init_dsa(TpStringHashTable *ht, dsa_area *area, uint32 initial_buckets)
{
	dsa_pointer *buckets;
	dsa_pointer buckets_dp;
	uint32 i;
	
	Assert(ht != NULL);
	Assert(area != NULL);
	Assert(initial_buckets > 0);
	
	/* Ensure bucket count is power of 2 */
	if ((initial_buckets & (initial_buckets - 1)) != 0)
	{
		/* Round up to next power of 2 */
		initial_buckets--;
		initial_buckets |= initial_buckets >> 1;
		initial_buckets |= initial_buckets >> 2;
		initial_buckets |= initial_buckets >> 4;
		initial_buckets |= initial_buckets >> 8;
		initial_buckets |= initial_buckets >> 16;
		initial_buckets++;
	}
	
	/* Allocate bucket array */
	buckets_dp = dsa_allocate(area, initial_buckets * sizeof(dsa_pointer));
	buckets = dsa_get_address(area, buckets_dp);
	
	/* Initialize hash table */
	ht->bucket_count = initial_buckets;
	ht->entry_count = 0;
	ht->collision_count = 0;
	ht->max_entries = 0; /* Not enforced for now */
	ht->buckets_dp = buckets_dp;
	
	/* Initialize all buckets to empty */
	for (i = 0; i < initial_buckets; i++)
		buckets[i] = InvalidDsaPointer;
		
	elog(DEBUG2, "Initialized DSA string table: %u buckets", initial_buckets);
}

/*
 * Clear an existing hash table, removing all entries
 * Note: This doesn't free DSA memory, just makes entries unreachable
 */
void
tp_hash_table_clear_dsa(dsa_area *area, TpStringHashTable *ht)
{
	dsa_pointer *buckets;
	uint32 i;
	
	Assert(ht != NULL);
	Assert(area != NULL);
	
	/* Get the bucket array */
	buckets = dsa_get_address(area, ht->buckets_dp);
	
	/* Reset statistics */
	ht->entry_count = 0;
	ht->collision_count = 0;
	
	/* Clear all buckets */
	for (i = 0; i < ht->bucket_count; i++)
		buckets[i] = InvalidDsaPointer;
		
	elog(DEBUG2, "Cleared DSA string table: %u buckets", ht->bucket_count);
}

/*
 * Look up a string in the DSA hash table
 * Returns NULL if not found
 */
TpStringHashEntry *
tp_hash_lookup_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len)
{
	uint32 hash;
	uint32 bucket;
	dsa_pointer entry_dp;
	TpStringHashEntry *entry;
	dsa_pointer *buckets;
	
	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);
	
	if (len == 0 || ht->entry_count == 0)
		return NULL;
		
	/* Calculate hash and bucket */
	hash = hash_bytes((const unsigned char *) str, len);
	bucket = hash & (ht->bucket_count - 1);
	
	/* Get bucket array */
	if (!DsaPointerIsValid(ht->buckets_dp))
		return NULL;
	buckets = dsa_get_address(area, ht->buckets_dp);
	
	/* Walk the bucket chain */
	entry_dp = buckets[bucket];
	while (DsaPointerIsValid(entry_dp))
	{
		entry = dsa_get_address(area, entry_dp);
		
		/* Check for match */
		if (entry->hash_value == hash &&
			entry->string_length == len &&
			memcmp(dsa_get_address(area, entry->string_dp), str, len) == 0)
		{
			return entry;
		}
		
		entry_dp = entry->next_dp;
	}
	
	return NULL;
}

/*
 * Insert a string into the DSA hash table
 * Returns the entry (existing or new)
 */
TpStringHashEntry *
tp_hash_insert_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len)
{
	uint32 hash;
	uint32 bucket;
	dsa_pointer entry_dp, string_dp;
	TpStringHashEntry *entry;
	TpStringHashEntry *new_entry;
	dsa_pointer *buckets;
	char *string_data;
	
	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);
	Assert(len > 0);
	
	/* First check if it already exists */
	entry = tp_hash_lookup_dsa(area, ht, str, len);
	if (entry != NULL)
		return entry;
	
	/* Calculate hash and bucket */
	hash = hash_bytes((const unsigned char *) str, len);
	bucket = hash & (ht->bucket_count - 1);
	
	/* Allocate new entry and string storage */
	entry_dp = dsa_allocate(area, sizeof(TpStringHashEntry));
	string_dp = dsa_allocate(area, len);
	
	new_entry = dsa_get_address(area, entry_dp);
	string_data = dsa_get_address(area, string_dp);
	
	/* Copy string data */
	memcpy(string_data, str, len);
	
	/* Initialize entry */
	new_entry->next_dp = InvalidDsaPointer;
	new_entry->string_dp = string_dp;
	new_entry->string_length = len;
	new_entry->hash_value = hash;
	new_entry->posting_list_dp = InvalidDsaPointer;
	new_entry->doc_freq = 0;
	
	/* Get bucket array and insert at head of chain */
	buckets = dsa_get_address(area, ht->buckets_dp);
	if (DsaPointerIsValid(buckets[bucket]))
		ht->collision_count++;
	
	new_entry->next_dp = buckets[bucket];
	buckets[bucket] = entry_dp;
	
	/* Update table statistics */
	ht->entry_count++;
	
	elog(DEBUG3, "Inserted DSA string entry: '%.*s' (len=%zu, hash=%u, bucket=%u)", 
		 (int)len, str, len, hash, bucket);
	
	return new_entry;
}

/*
 * Delete a string from the DSA hash table
 * Returns true if found and deleted, false if not found
 */
bool
tp_hash_delete_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len)
{
	uint32 hash;
	uint32 bucket;
	dsa_pointer entry_dp, prev_dp;
	TpStringHashEntry *entry, *prev_entry;
	dsa_pointer *buckets;
	
	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);
	
	if (len == 0 || ht->entry_count == 0)
		return false;
		
	/* Calculate hash and bucket */
	hash = hash_bytes((const unsigned char *) str, len);
	bucket = hash & (ht->bucket_count - 1);
	
	/* Get bucket array */
	if (!DsaPointerIsValid(ht->buckets_dp))
		return false;
	buckets = dsa_get_address(area, ht->buckets_dp);
	
	/* Find the entry in the chain */
	entry_dp = buckets[bucket];
	prev_dp = InvalidDsaPointer;
	
	while (DsaPointerIsValid(entry_dp))
	{
		entry = dsa_get_address(area, entry_dp);
		
		/* Check for match */
		if (entry->hash_value == hash &&
			entry->string_length == len &&
			memcmp(dsa_get_address(area, entry->string_dp), str, len) == 0)
		{
			/* Found it - remove from chain */
			if (DsaPointerIsValid(prev_dp))
			{
				prev_entry = dsa_get_address(area, prev_dp);
				prev_entry->next_dp = entry->next_dp;
			}
			else
			{
				/* First in chain */
				buckets[bucket] = entry->next_dp;
			}
			
			/* Free the string data and entry */
			dsa_free(area, entry->string_dp);
			dsa_free(area, entry_dp);
			
			/* Update statistics */
			ht->entry_count--;
			
			elog(DEBUG3, "Deleted DSA string entry: '%.*s'", (int)len, str);
			return true;
		}
		
		prev_dp = entry_dp;
		entry_dp = entry->next_dp;
	}
	
	return false;
}

/*
 * Resize the DSA hash table to a new bucket count
 * Returns true if successful, false if failed
 */
bool
tp_hash_resize_dsa(dsa_area *area, TpStringHashTable *ht, uint32 new_bucket_count)
{
	dsa_pointer old_buckets_dp, new_buckets_dp;
	dsa_pointer *old_buckets, *new_buckets;
	uint32 old_bucket_count;
	uint32 i;
	
	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(new_bucket_count > 0);
	
	/* Ensure new bucket count is power of 2 */
	if ((new_bucket_count & (new_bucket_count - 1)) != 0)
	{
		/* Round up to next power of 2 */
		new_bucket_count--;
		new_bucket_count |= new_bucket_count >> 1;
		new_bucket_count |= new_bucket_count >> 2;
		new_bucket_count |= new_bucket_count >> 4;
		new_bucket_count |= new_bucket_count >> 8;
		new_bucket_count |= new_bucket_count >> 16;
		new_bucket_count++;
	}
	
	if (new_bucket_count == ht->bucket_count)
		return true; /* Nothing to do */
	
	/* Allocate new bucket array */
	new_buckets_dp = dsa_allocate(area, new_bucket_count * sizeof(dsa_pointer));
	new_buckets = dsa_get_address(area, new_buckets_dp);
	
	/* Initialize new buckets */
	for (i = 0; i < new_bucket_count; i++)
		new_buckets[i] = InvalidDsaPointer;
	
	/* Save old bucket info */
	old_buckets_dp = ht->buckets_dp;
	old_buckets = dsa_get_address(area, old_buckets_dp);
	old_bucket_count = ht->bucket_count;
	
	/* Update table to use new buckets */
	ht->buckets_dp = new_buckets_dp;
	ht->bucket_count = new_bucket_count;
	ht->collision_count = 0; /* Will be recalculated */
	
	/* Rehash all entries */
	for (i = 0; i < old_bucket_count; i++)
	{
		dsa_pointer entry_dp = old_buckets[i];
		
		while (DsaPointerIsValid(entry_dp))
		{
			TpStringHashEntry *entry = dsa_get_address(area, entry_dp);
			dsa_pointer next_dp = entry->next_dp;
			uint32 new_bucket = entry->hash_value & (new_bucket_count - 1);
			
			/* Insert into new bucket */
			if (DsaPointerIsValid(new_buckets[new_bucket]))
				ht->collision_count++;
			entry->next_dp = new_buckets[new_bucket];
			new_buckets[new_bucket] = entry_dp;
			
			entry_dp = next_dp;
		}
	}
	
	/* Free old bucket array */
	dsa_free(area, old_buckets_dp);
	
	elog(DEBUG2, "Resized DSA string table: %u -> %u buckets", old_bucket_count, new_bucket_count);
	return true;
}
