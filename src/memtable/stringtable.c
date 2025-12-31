/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * stringtable.c - String interning hash table using dshash
 *
 * Provides efficient string storage with concurrent access. Strings are
 * stored in DSA memory and referenced by dsa_pointer keys.
 */
#include <postgres.h>

#include <lib/dshash.h>
#include <miscadmin.h>
#include <utils/memutils.h>

#include "common/hashfn.h"
#include "common/hashfn_unstable.h"
#include "memory.h"
#include "memtable.h"
#include "posting.h"
#include "state/state.h"
#include "stringtable.h"

/*
 * Hash function for variant string keys (char* or dsa_pointer)
 */
static dshash_hash
tp_string_hash_function(const void *key, size_t keysize, void *arg)
{
	const TpStringKey *string_key = (const TpStringKey *)key;
	dsa_area		  *area		  = (dsa_area *)arg;
	const char		  *str;
	dshash_hash		   hash_result;

	Assert(keysize == sizeof(TpStringKey));

	str = tp_get_key_str(area, string_key);

	/* Hash the null-terminated string content */
	hash_result = (dshash_hash)
			hash_bytes((const unsigned char *)str, strlen(str));

	return hash_result;
}

const char *
tp_get_key_str(dsa_area *area, const TpStringKey *key)
{
	if (key->posting_list == InvalidDsaPointer)
		return key->term.str;
	else
		return (const char *)dsa_get_address(area, key->term.dp);
}

/*
 * Compare function for variant string keys (char* or dsa_pointer)
 */
static int
tp_string_compare_function(
		const void *a, const void *b, size_t keysize, void *arg)
{
	const TpStringKey *key_a = (const TpStringKey *)a;
	const TpStringKey *key_b = (const TpStringKey *)b;
	dsa_area		  *area	 = (dsa_area *)arg;
	const char		  *str_a = tp_get_key_str(area, key_a);
	const char		  *str_b = tp_get_key_str(area, key_b);
	int				   result;

	Assert(keysize == sizeof(TpStringKey));

	result = strcmp(str_a, str_b);

	return result;
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
 * Create and initialize a new string hash table using dshash.  The hash table
 * contents live in the DSA area, but the returned handle is allocated from
 * backend memory using the current memory context.
 */
dshash_table *
tp_string_table_create(dsa_area *area)
{
	dshash_parameters params;

	/* Set up dshash parameters */
	params.key_size			= sizeof(TpStringKey);
	params.entry_size		= sizeof(TpStringHashEntry);
	params.hash_function	= tp_string_hash_function;
	params.compare_function = tp_string_compare_function;
	params.copy_function	= tp_string_copy_function;
	params.tranche_id		= TP_STRING_HASH_TRANCHE_ID;

	/* Create the dshash table */
	return dshash_create(area, &params, area);
}

/*
 * Attach to an existing string hash table using its handle.  Returns an object
 * allocated from backend memory using the current memory context to can be
 * used to access the hash table stored in the DSA area.
 */
dshash_table *
tp_string_table_attach(dsa_area *area, dshash_table_handle handle)
{
	dshash_parameters params;

	/* Set up dshash parameters */
	params.key_size			= sizeof(TpStringKey);
	params.entry_size		= sizeof(TpStringHashEntry);
	params.hash_function	= tp_string_hash_function;
	params.compare_function = tp_string_compare_function;
	params.copy_function	= tp_string_copy_function;
	params.tranche_id		= TP_STRING_HASH_TRANCHE_ID;

	/* Attach to the dshash table */
	return dshash_attach(area, &params, handle, area);
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

	string_dp = dsa_allocate(area, len + 1);
	if (!DsaPointerIsValid(string_dp))
		elog(ERROR, "Failed to allocate string in DSA");

	string_data = (char *)dsa_get_address(area, string_dp);

	/* Copy string data and null terminate */
	memcpy(string_data, str, len);
	string_data[len] = '\0';

	return string_dp;
}

/*
 * Look up a string in the hash table
 * Returns NULL if not found
 */
TpStringHashEntry *
tp_string_table_lookup(
		dsa_area *area, dshash_table *ht, const char *str, size_t len)
{
	TpStringKey		   lookup_key;
	TpStringHashEntry *entry;

	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);

	if (len == 0)
		return NULL;

	/* Build lookup key */
	lookup_key.term.str		= str;
	lookup_key.posting_list = InvalidDsaPointer;

	/* Look up using the stack-allocated key */
	entry = (TpStringHashEntry *)dshash_find(ht, &lookup_key, false);

	if (entry)
	{
		/* Release the lock acquired by dshash_find.
		 *
		 * SAFETY: The per-index LWLock ensures exclusive access during writes
		 * and prevents concurrent destruction of the hash table.
		 */
		dshash_release_lock(ht, entry);
	}

	return entry;
}

/*
 * Insert a string into the hash table
 * Returns the entry (existing or new)
 */
TpStringHashEntry *
tp_string_table_insert(
		dsa_area *area, dshash_table *ht, const char *str, size_t len)
{
	TpStringKey		   lookup_key;
	TpStringHashEntry *entry;
	bool			   found;

	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(str != NULL);
	Assert(len > 0);

	/* Build lookup key */
	lookup_key.term.str		= str;
	lookup_key.posting_list = InvalidDsaPointer;

	/* Try to find or insert the entry */
	entry = (TpStringHashEntry *)
			dshash_find_or_insert(ht, &lookup_key, &found);

	if (!found)
	{
		/* New entry */
		entry->key.term.dp		= tp_alloc_string_dsa(area, str, len);
		entry->key.posting_list = tp_alloc_posting_list(area);
	}

	/* Release the lock acquired by dshash_find_or_insert */
	dshash_release_lock(ht, entry);

	return entry;
}

/*
 * Clear the hash table, removing all entries
 * Frees all DSA string allocations
 */
void
tp_string_table_clear(dsa_area *area, dshash_table *ht)
{
	dshash_seq_status  status;
	TpStringHashEntry *entry;

	Assert(area != NULL);
	Assert(ht != NULL);

	/* Iterate through all entries and delete them */
	dshash_seq_init(&status, ht, true); /* exclusive for deletion */

	while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) != NULL)
	{
		/* Free the string */
		dsa_free(area, entry->key.term.dp);

		/* Free posting list */
		tp_free_posting_list(area, entry->key.posting_list);

		/* Delete current entry */
		dshash_delete_current(&status);
	}

	dshash_seq_term(&status);
}

/*
 * Get posting list for a term via string table lookup
 * Returns NULL if term not found
 */
TpPostingList *
tp_string_table_get_posting_list(
		dsa_area *area, dshash_table *ht, const char *term)
{
	TpStringHashEntry *entry;
	size_t			   term_len;

	Assert(area != NULL);
	Assert(ht != NULL);
	Assert(term != NULL);

	term_len = strlen(term);
	entry	 = tp_string_table_lookup(area, ht, term, term_len);

	if (entry && DsaPointerIsValid(entry->key.posting_list))
	{
		return (TpPostingList *)dsa_get_address(area, entry->key.posting_list);
	}

	return NULL;
}

/*
 * Get posting list for a specific term
 * Returns NULL if term not found
 */
TpPostingList *
tp_get_posting_list(TpLocalIndexState *local_state, const char *term)
{
	TpMemtable		  *memtable;
	dshash_table	  *string_table;
	TpStringHashEntry *string_entry;
	TpPostingList	  *posting_list;
	size_t			   term_len;

	Assert(local_state != NULL);
	Assert(term != NULL);

	/* Get memtable from local state */
	memtable = get_memtable(local_state);
	if (!memtable)
	{
		elog(ERROR, "Cannot get memtable - index state corrupted");
		return NULL; /* Never reached */
	}

	/* Get the string hash table from the memtable */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
		return NULL;

	string_table = tp_string_table_attach(
			local_state->dsa, memtable->string_hash_handle);
	if (!string_table)
	{
		elog(WARNING, "Failed to attach to string hash table");
		return NULL;
	}

	term_len = strlen(term);

	/* Look up the term in the string table */
	string_entry = tp_string_table_lookup(
			local_state->dsa, string_table, term, term_len);

	if (string_entry && DsaPointerIsValid(string_entry->key.posting_list))
	{
		posting_list = dsa_get_address(
				local_state->dsa, string_entry->key.posting_list);
		dshash_detach(string_table);
		return posting_list;
	}
	else
	{
		dshash_detach(string_table);
		return NULL;
	}
}

/*
 * Get or create a posting list for a term
 * This function manages the coordination between string table and posting
 * lists
 */
TpPostingList *
tp_get_or_create_posting_list(TpLocalIndexState *local_state, const char *term)
{
	TpMemtable		  *memtable;
	dshash_table	  *string_table;
	TpStringHashEntry *string_entry;
	TpPostingList	  *posting_list;
	dsa_pointer		   posting_list_dp;
	size_t			   term_len;

	Assert(local_state != NULL);
	Assert(term != NULL);

	term_len = strlen(term);

	/* Get memtable from local state */
	memtable = get_memtable(local_state);
	if (!memtable)
	{
		elog(ERROR, "Cannot get memtable");
		return NULL;
	}

	/* Initialize string hash table if needed */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		/* Create new dshash table */
		string_table = tp_string_table_create(local_state->dsa);
		if (!string_table)
		{
			elog(ERROR, "Failed to create string hash table");
			return NULL;
		}

		/* Store the handle for other processes */
		memtable->string_hash_handle = dshash_get_hash_table_handle(
				string_table);
	}
	else
	{
		/* Attach to existing table */
		string_table = tp_string_table_attach(
				local_state->dsa, memtable->string_hash_handle);
		if (!string_table)
		{
			elog(ERROR, "Failed to attach to string hash table");
			return NULL;
		}
	}

	/* Look up or insert the term in the string table */
	string_entry = tp_string_table_lookup(
			local_state->dsa, string_table, term, term_len);

	if (!string_entry)
	{
		/* Insert the term */
		string_entry = tp_string_table_insert(
				local_state->dsa, string_table, term, term_len);
		if (!string_entry)
		{
			elog(ERROR, "Failed to insert term '%s' into string table", term);
			return NULL;
		}
	}

	/* Check if posting list already exists for this term */
	if (DsaPointerIsValid(string_entry->key.posting_list))
	{
		posting_list = dsa_get_address(
				local_state->dsa, string_entry->key.posting_list);
	}
	else
	{
		/* Create new posting list */
		posting_list_dp = tp_alloc_posting_list(local_state->dsa);
		posting_list	= dsa_get_address(local_state->dsa, posting_list_dp);

		/* Associate posting list with string entry */
		string_entry->key.posting_list = posting_list_dp;
	}

	/* Detach from string table */
	dshash_detach(string_table);

	return posting_list;
}

/*
 * Add terms from a document to the posting lists
 * This coordinates between string table and posting list management
 */
void
tp_add_document_terms(
		TpLocalIndexState *local_state,
		ItemPointer		   ctid,
		char			 **terms,
		int32			  *frequencies,
		int				   term_count,
		int32			   doc_length)
{
	int i;

	for (i = 0; i < term_count; i++)
	{
		TpPostingList *posting_list;
		int32		   frequency = frequencies[i];

		/* Get or create posting list for this term */
		posting_list = tp_get_or_create_posting_list(local_state, terms[i]);

		/* Add document entry to posting list */
		tp_add_document_to_posting_list(
				local_state, posting_list, ctid, frequency);
	}

	/* Store document length in the document length table */
	tp_store_document_length(local_state, ctid, doc_length);

	/*
	 * Update corpus statistics.
	 * Protected by the per-index LWLock acquired at transaction level.
	 * The lock's memory barriers ensure these updates are visible to other
	 * backends on NUMA systems.
	 */
	local_state->shared->total_docs++;
	local_state->shared->total_len += doc_length;

	/* Track terms added in this transaction for bulk load detection */
	local_state->terms_added_this_xact += term_count;
}
