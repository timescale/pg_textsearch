/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * stringtable.c — string interning hash table using dshash, for
 * the in-memory memtable cache.
 *
 * Strings are stored in DSA memory and referenced by dsa_pointer
 * keys.  Provides concurrent term lookup with a variant key that
 * compares char* (for lookup) and dsa_pointer (for storage) under
 * the same hash function.  See docs/memtable_cache.md.
 */
#include <postgres.h>

#include <lib/dshash.h>
#include <miscadmin.h>
#include <utils/memutils.h>

#include "common/hashfn.h"
#include "common/hashfn_unstable.h"
#include "index/state.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"

/*
 * Get string length from key.
 * For lookup keys (posting_list == Invalid), use the stored len field.
 * For stored keys, use strlen on the null-terminated DSA string.
 */
static inline size_t
tp_get_key_len(dsa_area *area, const TpStringKey *key)
{
	if (key->posting_list == InvalidDsaPointer)
		return key->len;
	else
		return strlen((const char *)dsa_get_address(area, key->term.dp));
}

/*
 * Hash function for variant string keys (char* or dsa_pointer)
 */
static dshash_hash
tp_string_hash_function(const void *key, size_t keysize, void *arg)
{
	const TpStringKey *string_key = (const TpStringKey *)key;
	dsa_area		  *area		  = (dsa_area *)arg;
	const char		  *str;
	size_t			   len;
	dshash_hash		   hash_result;

	Assert(keysize == sizeof(TpStringKey));
	(void)keysize;

	str = tp_get_key_str(area, string_key);
	len = tp_get_key_len(area, string_key);

	/* Hash the string content using explicit length (no strlen needed) */
	hash_result = (dshash_hash)hash_bytes((const unsigned char *)str, len);

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
 * Uses explicit lengths to avoid requiring null-terminated strings for
 * lookups.
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
	size_t			   len_a = tp_get_key_len(area, key_a);
	size_t			   len_b = tp_get_key_len(area, key_b);
	int				   result;

	Assert(keysize == sizeof(TpStringKey));
	(void)keysize;

	/* Compare by length first for efficiency */
	if (len_a != len_b)
		return (len_a < len_b) ? -1 : 1;

	/* Same length - compare contents */
	result = memcmp(str_a, str_b, len_a);

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
	(void)keysize;
	*((TpStringKey *)dest) = *((TpStringKey *)src);
}

/*
 * Create and initialize a new string hash table using dshash.  The hash table
 * contents live in the DSA area, but the returned handle is allocated from
 * backend memory using the current memory context.
 */
static dshash_table *
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
	(void)area;
	Assert(ht != NULL);
	Assert(str != NULL);

	if (len == 0)
		return NULL;

	/* Build lookup key with explicit length (no null termination required) */
	lookup_key.term.str		= str;
	lookup_key.posting_list = InvalidDsaPointer;
	lookup_key.len			= len;

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
 * Ensure the memtable's string hash table is initialized.
 *
 * Must be called under LW_EXCLUSIVE on the per-index lock.
 * This is the cold path — only needed once per memtable
 * lifecycle (after creation or spill). Separated from
 * tp_get_or_create_posting_list so the hot path (which
 * runs under LW_SHARED) never races on hash table creation.
 */
void
tp_ensure_string_table_initialized(TpLocalIndexState *local_state)
{
	TpMemtable	 *memtable;
	dshash_table *table;

	Assert(local_state != NULL);

	memtable = get_memtable(local_state);
	if (!memtable)
		return;

	/* Initialize string hash table if needed */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		table = tp_string_table_create(local_state->dsa);
		if (!table)
			elog(ERROR, "Failed to create string hash table");
		memtable->string_hash_handle = dshash_get_hash_table_handle(table);
		dshash_detach(table);
	}

	/* Initialize doc lengths hash table if needed */
	if (memtable->doc_lengths_handle == DSHASH_HANDLE_INVALID)
	{
		table = tp_doclength_table_create(local_state->dsa);
		if (!table)
			elog(ERROR, "Failed to create doc length table");
		memtable->doc_lengths_handle = dshash_get_hash_table_handle(table);
		dshash_detach(table);
	}
}

/*
 * Get or create a posting list for a term.
 *
 * Uses a single dshash_find_or_insert call to atomically find or
 * create the entry, eliminating the lookup-then-insert race that
 * exists when two backends see "not found" for the same term.
 *
 * Caller must have ensured the string hash table is initialized
 * (via tp_ensure_string_table_initialized under LW_EXCLUSIVE).
 */
static TpPostingList *
tp_get_or_create_posting_list(TpLocalIndexState *local_state, const char *term)
{
	TpMemtable		  *memtable;
	dshash_table	  *string_table;
	TpStringHashEntry *entry;
	TpStringKey		   lookup_key;
	TpPostingList	  *posting_list;
	size_t			   term_len;
	bool			   found;

	Assert(local_state != NULL);
	Assert(term != NULL);

	term_len = strlen(term);
	memtable = get_memtable(local_state);
	if (!memtable)
	{
		elog(ERROR, "Cannot get memtable");
		return NULL;
	}

	/*
	 * The string hash table must already be initialized
	 * by tp_ensure_string_table_initialized (called under
	 * LW_EXCLUSIVE). In build mode it's created lazily
	 * since there's no concurrency.
	 */
	if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
	{
		if (!local_state->is_build_mode)
			elog(ERROR,
				 "String hash table not initialized "
				 "(call tp_ensure_string_table_initialized "
				 "first)");

		/* Build mode: create lazily (single-threaded) */
		string_table = tp_string_table_create(local_state->dsa);
		if (!string_table)
			elog(ERROR, "Failed to create string hash table");
		memtable->string_hash_handle = dshash_get_hash_table_handle(
				string_table);
	}
	else
	{
		string_table = tp_string_table_attach(
				local_state->dsa, memtable->string_hash_handle);
		if (!string_table)
			elog(ERROR,
				 "Failed to attach to string hash "
				 "table");
	}

	/* Build lookup key */
	lookup_key.term.str		= term;
	lookup_key.posting_list = InvalidDsaPointer;
	lookup_key.len			= term_len;

	/* Atomic find-or-insert: holds exclusive partition lock */
	entry = (TpStringHashEntry *)
			dshash_find_or_insert(string_table, &lookup_key, &found);

	if (!found)
	{
		/* New term -- initialize under partition lock */
		entry->key.term.dp =
				tp_alloc_string_dsa(local_state->dsa, term, term_len);
		entry->key.posting_list = tp_alloc_posting_list(local_state->dsa);
	}

	posting_list = dsa_get_address(local_state->dsa, entry->key.posting_list);

	/* Release partition lock */
	dshash_release_lock(string_table, entry);
	dshash_detach(string_table);

	return posting_list;
}

/*
 * tp_cache_apply_document — apply a single doc record into the
 * in-memory cache structures (string table, posting lists, doc-
 * length table).
 *
 * Used by the cache apply protocol to bring the cache up to
 * date with the on-disk chain. The caller has already parsed a
 * (ctid, terms[], frequencies[], doc_length) tuple out of a
 * chain doc record.
 *
 * Idempotent by CTID: tp_store_document_length is the single
 * check-and-add gate.  If the CTID is already in the doclength
 * table (e.g., another applier raced us), we return without
 * touching the posting lists.  Corpus statistics
 * (TpSharedIndexState.total_docs / total_len) are NOT updated
 * here — those are owned by the on-disk write path
 * (tp_add_document_terms in src/memtable/log.c) and reflect the
 * chain, not the cache.  Bulk-load counter is also untouched
 * for the same reason.
 */
void
tp_cache_apply_document(
		TpLocalIndexState *local_state,
		ItemPointer		   ctid,
		char			 **terms,
		int32			  *frequencies,
		int				   term_count,
		int32			   doc_length)
{
	int i;

	if (!tp_store_document_length(local_state, ctid, doc_length))
		return;

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
}
