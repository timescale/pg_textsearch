/*-------------------------------------------------------------------------
 *
 * stringtable.c
 *	  String interning hash table using PostgreSQL's dshash
 *	  Provides efficient string storage with concurrent access
 *
 * This implementation uses dshash for the hash table structure while
 * maintaining the original API. Strings are stored in DSA memory and
 * referenced by dsa_pointer keys in the hash table.
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
#include "common/hashfn_unstable.h"
#include "lib/dshash.h"

/*
 * Hash function for string pointers stored in DSA
 * Dereferences the pointer and hashes the string content
 */
static dshash_hash
tp_string_hash_function(const void *key, size_t keysize, void *arg)
{
	const dsa_pointer *string_dp = (const dsa_pointer *) key;
	dsa_area *area = (dsa_area *) arg;
	const char *str;
	uint32 len;

	Assert(keysize == sizeof(dsa_pointer));
	Assert(DsaPointerIsValid(*string_dp));

	/* Get the string from DSA */
	str = (const char *) dsa_get_address(area, *string_dp);
	
	/* String is stored with length prefix */
	len = *((uint32 *) str);
	str += sizeof(uint32);

	/* Hash the string content */
	return (dshash_hash) hash_bytes((const unsigned char *) str, len);
}

/*
 * Compare function for string pointers stored in DSA
 * Dereferences both pointers and compares string content
 */
static int
tp_string_compare_function(const void *a, const void *b, size_t keysize, void *arg)
{
	const dsa_pointer *string_dp_a = (const dsa_pointer *) a;
	const dsa_pointer *string_dp_b = (const dsa_pointer *) b;
	dsa_area *area = (dsa_area *) arg;
	const char *str_a, *str_b;
	uint32 len_a, len_b;

	Assert(keysize == sizeof(dsa_pointer));

	/* Handle identical pointers (same string) */
	if (*string_dp_a == *string_dp_b)
		return 0;

	/* Both pointers must be valid */
	Assert(DsaPointerIsValid(*string_dp_a));
	Assert(DsaPointerIsValid(*string_dp_b));

	/* Get the strings from DSA */
	str_a = (const char *) dsa_get_address(area, *string_dp_a);
	str_b = (const char *) dsa_get_address(area, *string_dp_b);
	
	/* Extract length prefixes */
	len_a = *((uint32 *) str_a);
	len_b = *((uint32 *) str_b);
	str_a += sizeof(uint32);
	str_b += sizeof(uint32);

	/* Compare lengths first */
	if (len_a != len_b)
		return (len_a < len_b) ? -1 : 1;

	/* Compare string content */
	return memcmp(str_a, str_b, len_a);
}

/*
 * Copy function for string pointers
 * Simple pointer copy since we're using DSA pointers as keys
 */
static void
tp_string_copy_function(void *dest, const void *src, size_t keysize, void *arg)
{
	Assert(keysize == sizeof(dsa_pointer));
	*((dsa_pointer *) dest) = *((dsa_pointer *) src);
}

/*
 * Create and initialize a new string hash table using dshash
 */
TpStringHashTable *
tp_hash_table_create_dsa(dsa_area *area, uint32 initial_buckets)
{
	TpStringHashTable *ht;
	dshash_parameters params;

	Assert(area != NULL);

	/* Set up dshash parameters */
	params.key_size = sizeof(dsa_pointer);
	params.entry_size = sizeof(TpStringHashEntry);
	params.hash_function = tp_string_hash_function;
	params.compare_function = tp_string_compare_function;
	params.copy_function = tp_string_copy_function;
	params.tranche_id = TP_STRING_HASH_TRANCHE_ID;

	/* Allocate wrapper structure */
	ht = (TpStringHashTable *) MemoryContextAlloc(TopMemoryContext, 
													sizeof(TpStringHashTable));

	/* Create the dshash table */
	ht->dshash = dshash_create(area, &params, area);
	ht->handle = dshash_get_hash_table_handle(ht->dshash);
	ht->entry_count = 0;
	ht->max_entries = 0;

	elog(DEBUG2, "Created dshash string table with handle %lu", 
		 (unsigned long) ht->handle);

	return ht;
}

/*
 * Attach to an existing string hash table using its handle
 */
TpStringHashTable *
tp_hash_table_attach_dsa(dsa_area *area, dshash_table_handle handle)
{
	TpStringHashTable *ht;
	dshash_parameters params;

	Assert(area != NULL);
	Assert(handle != DSHASH_HANDLE_INVALID);

	/* Set up dshash parameters */
	params.key_size = sizeof(dsa_pointer);
	params.entry_size = sizeof(TpStringHashEntry);
	params.hash_function = tp_string_hash_function;
	params.compare_function = tp_string_compare_function;
	params.copy_function = tp_string_copy_function;
	params.tranche_id = TP_STRING_HASH_TRANCHE_ID;

	/* Allocate wrapper structure */
	ht = (TpStringHashTable *) MemoryContextAlloc(TopMemoryContext, 
													sizeof(TpStringHashTable));

	/* Attach to the dshash table */
	ht->dshash = dshash_attach(area, &params, handle, area);
	ht->handle = handle;
	ht->entry_count = 0; /* We don't track this accurately */
	ht->max_entries = 0;

	elog(DEBUG2, "Attached to dshash string table with handle %lu", 
		 (unsigned long) handle);

	return ht;
}

/*
 * Detach from a string hash table
 */
void
tp_hash_table_detach_dsa(TpStringHashTable *ht)
{
	Assert(ht != NULL);
	Assert(ht->dshash != NULL);

	dshash_detach(ht->dshash);
	pfree(ht);

	elog(DEBUG2, "Detached from dshash string table");
}

/*
 * Destroy a string hash table
 */
void
tp_hash_table_destroy_dsa(TpStringHashTable *ht)
{
	Assert(ht != NULL);
	Assert(ht->dshash != NULL);

	dshash_destroy(ht->dshash);
	pfree(ht);

	elog(DEBUG2, "Destroyed dshash string table");
}

/*
 * Get the handle for sharing the table across processes
 */
dshash_table_handle
tp_hash_table_get_handle(TpStringHashTable *ht)
{
	Assert(ht != NULL);
	return ht->handle;
}

/*
 * Allocate a string in DSA memory with length prefix
 * Returns the dsa_pointer to the allocated string
 */
static dsa_pointer
tp_alloc_string_dsa(dsa_area *area, const char *str, size_t len)
{
	dsa_pointer string_dp;
	char *string_data;
	uint32 *length_ptr;

	/* Allocate space for length prefix + string */
	string_dp = dsa_allocate(area, sizeof(uint32) + len);
	string_data = (char *) dsa_get_address(area, string_dp);

	/* Store length prefix */
	length_ptr = (uint32 *) string_data;
	*length_ptr = len;

	/* Copy string data */
	memcpy(string_data + sizeof(uint32), str, len);

	return string_dp;
}

/*
 * Look up a string in the hash table
 * Returns NULL if not found
 * 
 * Creates a temporary string allocation to use as the lookup key.
 * dshash will use our custom hash/compare functions that dereference 
 * the pointer and compare actual string content.
 */
TpStringHashEntry *
tp_hash_lookup_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len)
{
	dsa_pointer temp_string_dp;
	TpStringHashEntry *entry;

	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);

	if (len == 0)
		return NULL;

	/* Create temporary string allocation to use as lookup key */
	temp_string_dp = tp_alloc_string_dsa(area, str, len);

	/* Look up using the temporary string pointer as key */
	entry = (TpStringHashEntry *) dshash_find(ht->dshash, &temp_string_dp, false);

	/* Free the temporary string */
	dsa_free(area, temp_string_dp);

	if (entry)
	{
		/* Release the lock acquired by dshash_find */
		dshash_release_lock(ht->dshash, entry);
	}

	return entry;
}

/*
 * Insert a string into the hash table
 * Returns the entry (existing or new)
 * 
 * Creates a temporary string allocation for lookup. If not found,
 * creates a permanent allocation and stores it in the entry.
 */
TpStringHashEntry *
tp_hash_insert_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len)
{
	dsa_pointer temp_string_dp;
	TpStringHashEntry *entry;
	bool found;

	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);
	Assert(len > 0);

	/* Create temporary string allocation to use as lookup key */
	temp_string_dp = tp_alloc_string_dsa(area, str, len);

	/* Try to find or insert using temporary string as key */
	entry = (TpStringHashEntry *) dshash_find_or_insert(ht->dshash, &temp_string_dp, &found);

	if (!found)
	{
		/* New entry - use the temporary allocation as the permanent one */
		uint32 hash_value = hash_bytes((const unsigned char *) str, len);
		
		entry->string_dp = temp_string_dp;
		entry->string_length = len;
		entry->hash_value = hash_value;
		entry->posting_list_dp = InvalidDsaPointer;
		entry->doc_freq = 0;

		ht->entry_count++;

		elog(DEBUG3, "Inserted new string entry: '%.*s' (len=%zu, hash=%u)", 
			 (int)len, str, len, hash_value);
	}
	else
	{
		/* Found existing entry - free the temporary allocation */
		dsa_free(area, temp_string_dp);
		
		elog(DEBUG3, "Found existing string entry: '%.*s' (len=%zu)", 
			 (int)len, str, len);
	}

	/* Release the lock acquired by dshash_find_or_insert */
	dshash_release_lock(ht->dshash, entry);

	return entry;
}

/*
 * Delete a string from the hash table
 * Returns true if found and deleted, false if not found
 */
bool
tp_hash_delete_dsa(dsa_area *area, TpStringHashTable *ht, const char *str, size_t len)
{
	dsa_pointer temp_string_dp;
	TpStringHashEntry *entry;

	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);

	if (len == 0)
		return false;

	/* Create temporary string allocation to use as lookup key */
	temp_string_dp = tp_alloc_string_dsa(area, str, len);

	/* Find the entry using temporary string as key */
	entry = (TpStringHashEntry *) dshash_find(ht->dshash, &temp_string_dp, true); /* exclusive lock */
	
	/* Free the temporary string */
	dsa_free(area, temp_string_dp);

	if (entry)
	{
		/* Found the entry - delete it */
		dsa_free(area, entry->string_dp);  /* Free the string data first */
		dshash_delete_entry(ht->dshash, entry);  /* This releases the lock too */
		
		ht->entry_count--;
		elog(DEBUG3, "Deleted string entry: '%.*s'", (int)len, str);
		return true;
	}

	return false;
}

/*
 * Clear the hash table, removing all entries
 * Note: This doesn't free individual string allocations
 */
void
tp_hash_table_clear_dsa(dsa_area *area, TpStringHashTable *ht)
{
	dshash_seq_status status;
	TpStringHashEntry *entry;

	Assert(area != NULL);
	Assert(ht != NULL);

	/* Iterate through all entries and delete them */
	dshash_seq_init(&status, ht->dshash, true); /* exclusive for deletion */

	while ((entry = (TpStringHashEntry *) dshash_seq_next(&status)) != NULL)
	{
		/* Free the string data */
		dsa_free(area, entry->string_dp);
		
		/* Delete current entry */
		dshash_delete_current(&status);
	}

	dshash_seq_term(&status);

	ht->entry_count = 0;

	elog(DEBUG2, "Cleared dshash string table");
}