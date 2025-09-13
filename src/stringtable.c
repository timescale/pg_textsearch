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

#include <postgres.h>

#include <lib/dshash.h>
#include <utils/memutils.h>

#include "common/hashfn.h"
#include "common/hashfn_unstable.h"
#include "stringtable.h"

/*
 * Hash function for variant string keys (char* or dsa_pointer)
 * Uses flag_field to distinguish: 0 = char*, non-0 = dsa_pointer
 */
static dshash_hash
tp_string_hash_function(const void *key, size_t keysize, void *arg)
{
	const TpStringKey *string_key = (const TpStringKey *)key;
	dsa_area		  *area		  = (dsa_area *)arg;
	const char		  *str;

	Assert(keysize == sizeof(TpStringKey));

	if (string_key->flag_field == 0)
	{
		/* Lookup key: string_or_ptr is a cast char* */
		str = (const char *)(uintptr_t)string_key->string_or_ptr;
	}
	else
	{
		/* Table entry: string_or_ptr is a dsa_pointer */
		Assert(DsaPointerIsValid(string_key->string_or_ptr));
		str = (const char *)dsa_get_address(area, string_key->string_or_ptr);
	}

	/* Hash the null-terminated string content */
	return (dshash_hash)hash_bytes((const unsigned char *)str, strlen(str));
}

/*
 * Compare function for variant string keys (char* or dsa_pointer)
 * Handles all combinations: char* vs char*, char* vs dsa_pointer, etc.
 */
static int
tp_string_compare_function(
		const void *a, const void *b, size_t keysize, void *arg)
{
	const TpStringKey *key_a = (const TpStringKey *)a;
	const TpStringKey *key_b = (const TpStringKey *)b;
	dsa_area		  *area	 = (dsa_area *)arg;
	const char		  *str_a, *str_b;

	Assert(keysize == sizeof(TpStringKey));

	/* Handle identical keys (same structure content) */
	if (key_a->string_or_ptr == key_b->string_or_ptr &&
		key_a->flag_field == key_b->flag_field)
		return 0;

	/* Get string A */
	if (key_a->flag_field == 0)
	{
		/* Lookup key: string_or_ptr is a cast char* */
		str_a = (const char *)(uintptr_t)key_a->string_or_ptr;
	}
	else
	{
		/* Table entry: string_or_ptr is a dsa_pointer */
		Assert(DsaPointerIsValid(key_a->string_or_ptr));
		str_a = (const char *)dsa_get_address(area, key_a->string_or_ptr);
	}

	/* Get string B */
	if (key_b->flag_field == 0)
	{
		/* Lookup key: string_or_ptr is a cast char* */
		str_b = (const char *)(uintptr_t)key_b->string_or_ptr;
	}
	else
	{
		/* Table entry: string_or_ptr is a dsa_pointer */
		Assert(DsaPointerIsValid(key_b->string_or_ptr));
		str_b = (const char *)dsa_get_address(area, key_b->string_or_ptr);
	}

	/* Compare null-terminated strings */
	return strcmp(str_a, str_b);
}

/*
 * Copy function for variant string keys
 * Simple structure copy
 */
static void
tp_string_copy_function(
		void	   *dest,
		const void *src,
		size_t		keysize,
		void	   *arg __attribute__((unused)))
{
	Assert(keysize == sizeof(TpStringKey));
	*((TpStringKey *)dest) = *((TpStringKey *)src);
}

/*
 * Create and initialize a new string hash table using dshash
 */
TpStringHashTable *
tp_hash_table_create_dsa(dsa_area *area)
{
	TpStringHashTable *ht;
	dshash_parameters  params;

	Assert(area != NULL);

	/* Set up dshash parameters */
	params.key_size			= sizeof(TpStringKey);
	params.entry_size		= sizeof(TpStringHashEntry);
	params.hash_function	= tp_string_hash_function;
	params.compare_function = tp_string_compare_function;
	params.copy_function	= tp_string_copy_function;
	params.tranche_id		= TP_STRING_HASH_TRANCHE_ID;

	/* Allocate wrapper structure */
	ht = (TpStringHashTable *)
			MemoryContextAlloc(TopMemoryContext, sizeof(TpStringHashTable));

	/* Create the dshash table */
	ht->dshash		= dshash_create(area, &params, area);
	ht->handle		= dshash_get_hash_table_handle(ht->dshash);
	ht->entry_count = 0;
	ht->max_entries = 0;

	return ht;
}

/*
 * Attach to an existing string hash table using its handle
 */
TpStringHashTable *
tp_hash_table_attach_dsa(dsa_area *area, dshash_table_handle handle)
{
	TpStringHashTable *ht;
	dshash_parameters  params;

	Assert(area != NULL);
	Assert(handle != DSHASH_HANDLE_INVALID);

	/* Set up dshash parameters */
	params.key_size			= sizeof(TpStringKey);
	params.entry_size		= sizeof(TpStringHashEntry);
	params.hash_function	= tp_string_hash_function;
	params.compare_function = tp_string_compare_function;
	params.copy_function	= tp_string_copy_function;
	params.tranche_id		= TP_STRING_HASH_TRANCHE_ID;

	/* Allocate wrapper structure */
	ht = (TpStringHashTable *)
			MemoryContextAlloc(TopMemoryContext, sizeof(TpStringHashTable));

	/* Attach to the dshash table */
	ht->dshash		= dshash_attach(area, &params, handle, area);
	ht->handle		= handle;
	ht->entry_count = 0; /* We don't track this accurately */
	ht->max_entries = 0;

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
 * Allocate a null-terminated string in DSA memory
 * Returns the dsa_pointer to the allocated string
 */
static dsa_pointer
tp_alloc_string_dsa(dsa_area *area, const char *str, size_t len)
{
	dsa_pointer string_dp;
	char	   *string_data;

	/* Allocate space for string + null terminator */
	string_dp	= dsa_allocate(area, len + 1);
	string_data = (char *)dsa_get_address(area, string_dp);

	/* Copy string data and null terminate */
	memcpy(string_data, str, len);
	string_data[len] = '\0';

	return string_dp;
}

/*
 * Look up a string in the hash table
 * Returns NULL if not found
 *
 * Uses zero-allocation approach: builds TpStringKey on stack with
 * char* cast as string_or_ptr and flag_field=0.
 */
TpStringHashEntry *
tp_hash_lookup_dsa(
		dsa_area *area, TpStringHashTable *ht, const char *str, size_t len)
{
	TpStringKey		   lookup_key;
	TpStringHashEntry *entry;

	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);

	if (len == 0)
		return NULL;

	/* Build lookup key on stack - no allocations! */
	lookup_key.string_or_ptr = (dsa_pointer)(uintptr_t)str;
	lookup_key.flag_field = 0; /* Indicates this is a char*, not dsa_pointer */

	/* Look up using the stack-allocated key */
	entry = (TpStringHashEntry *)dshash_find(ht->dshash, &lookup_key, false);

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
 * Uses zero-allocation approach for lookup. Only allocates DSA string
 * if creating a new entry.
 */
TpStringHashEntry *
tp_hash_insert_dsa(
		dsa_area *area, TpStringHashTable *ht, const char *str, size_t len)
{
	TpStringKey		   lookup_key;
	TpStringHashEntry *entry;
	bool			   found;

	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);
	Assert(len > 0);

	/* Build lookup key on stack - no allocations! */
	lookup_key.string_or_ptr = (dsa_pointer)(uintptr_t)str;
	lookup_key.flag_field = 0; /* Indicates this is a char*, not dsa_pointer */

	/* Try to find or insert using the stack-allocated key */
	entry = (TpStringHashEntry *)
			dshash_find_or_insert(ht->dshash, &lookup_key, &found);

	if (!found)
	{
		/* New entry - allocate DSA string and initialize */
		dsa_pointer string_dp  = tp_alloc_string_dsa(area, str, len);
		uint32		hash_value = hash_bytes((const unsigned char *)str, len);

		entry->key.string_or_ptr = string_dp;
		entry->key.flag_field =
				InvalidDsaPointer; /* Will be updated with posting_list_dp */
		entry->posting_list_len = 0;
		entry->hash_value		= hash_value;

		ht->entry_count++;
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
tp_hash_delete_dsa(
		dsa_area *area, TpStringHashTable *ht, const char *str, size_t len)
{
	TpStringKey		   lookup_key;
	TpStringHashEntry *entry;

	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);

	if (len == 0)
		return false;

	/* Build lookup key on stack - no allocations! */
	lookup_key.string_or_ptr = (dsa_pointer)(uintptr_t)str;
	lookup_key.flag_field = 0; /* Indicates this is a char*, not dsa_pointer */

	/* Find the entry using stack-allocated key */
	entry = (TpStringHashEntry *)
			dshash_find(ht->dshash, &lookup_key, true); /* exclusive lock */

	if (entry)
	{
		/* Found the entry - free DSA string and delete */
		dsa_free(
				area, entry->key.string_or_ptr); /* Free the DSA string data */
		dshash_delete_entry(
				ht->dshash, entry); /* This releases the lock too */

		ht->entry_count--;
		return true;
	}

	return false;
}

/*
 * Clear the hash table, removing all entries
 * Frees all DSA string allocations
 */
void
tp_hash_table_clear_dsa(dsa_area *area, TpStringHashTable *ht)
{
	dshash_seq_status  status;
	TpStringHashEntry *entry;

	Assert(area != NULL);
	Assert(ht != NULL);

	/* Iterate through all entries and delete them */
	dshash_seq_init(&status, ht->dshash, true); /* exclusive for deletion */

	while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) != NULL)
	{
		/* Free the DSA string data */
		dsa_free(area, entry->key.string_or_ptr);

		/* Delete current entry */
		dshash_delete_current(&status);
	}

	dshash_seq_term(&status);

	ht->entry_count = 0;
}
