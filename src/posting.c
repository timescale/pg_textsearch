/*-------------------------------------------------------------------------
 *
 * posting.c
 *	  Simplified posting list management for new architecture
 *
 * IDENTIFICATION
 *	  src/posting.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <lib/dshash.h>
#include <math.h>
#include <miscadmin.h>
#include <storage/itemptr.h>
#include <utils/dsa.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>

#include "common/hashfn.h"
#include "constants.h"
#include "memory.h"
#include "memtable.h"
#include "metapage.h"
#include "posting.h"
#include "state.h"
#include "stringtable.h"

/* Configuration parameters */
int tp_posting_list_growth_factor = TP_POSTING_LIST_GROWTH_FACTOR;

/*
 * Free a posting list and its entries array
 */
void
tp_free_posting_list(dsa_area *area, dsa_pointer posting_list_dp)
{
	TpPostingList *posting_list;

	if (!DsaPointerIsValid(posting_list_dp))
		return;

	posting_list = (TpPostingList *)dsa_get_address(area, posting_list_dp);

	/* Free entries array if it exists */
	if (DsaPointerIsValid(posting_list->entries_dp))
	{
		dsa_free(area, posting_list->entries_dp);
	}

	/* Free the posting list structure itself */
	dsa_free(area, posting_list_dp);
}

/* Helper function to get entries array from posting list */
TpPostingEntry *
tp_get_posting_entries(dsa_area *area, TpPostingList *posting_list)
{
	if (!posting_list || !DsaPointerIsValid(posting_list->entries_dp))
		return NULL;
	if (!area)
		return NULL;
	return dsa_get_address(area, posting_list->entries_dp);
}

/*
 * Allocate and initialize a new posting list in DSA
 * Returns the DSA pointer to the allocated posting list
 */
dsa_pointer
tp_alloc_posting_list(dsa_area *dsa, TpMemoryUsage *memory_usage)
{
	dsa_pointer	   posting_list_dp;
	TpPostingList *posting_list;

	Assert(dsa != NULL);
	Assert(memory_usage != NULL);

	/* Allocate posting list structure with tracking */
	posting_list_dp =
			tp_dsa_allocate(dsa, memory_usage, sizeof(TpPostingList));
	if (!DsaPointerIsValid(posting_list_dp))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("pg_textsearch index memory limit exceeded"),
				 errdetail(
						 "Current usage: %zu bytes, limit: %zu bytes",
						 tp_get_memory_usage(memory_usage),
						 tp_get_memory_limit()),
				 errhint("Increase pg_textsearch.index_memory_limit or "
						 "reduce the amount of data being indexed.")));
	}

	posting_list = dsa_get_address(dsa, posting_list_dp);

	/* Initialize posting list */
	memset(posting_list, 0, sizeof(TpPostingList));
	posting_list->doc_count	 = 0;
	posting_list->capacity	 = 0;
	posting_list->is_sorted	 = false;
	posting_list->doc_freq	 = 0;
	posting_list->entries_dp = InvalidDsaPointer;

	return posting_list_dp;
}

/*
 * Add a document entry to a posting list - simplified
 */
void
tp_add_document_to_posting_list(
		TpLocalIndexState *local_state,
		TpPostingList	  *posting_list,
		ItemPointer		   ctid,
		int32			   frequency)
{
	TpPostingEntry *entries;
	TpPostingEntry *new_entry;
	dsa_pointer		new_entries_dp;

	Assert(local_state != NULL);
	Assert(posting_list != NULL);
	Assert(ItemPointerIsValid(ctid));

	/* Expand array if needed */
	if (posting_list->doc_count >= posting_list->capacity)
	{
		int32 new_capacity = posting_list->capacity == 0
								   ? TP_INITIAL_POSTING_LIST_CAPACITY
								   : posting_list->capacity *
											 tp_posting_list_growth_factor;
		Size  old_size	   = posting_list->capacity * sizeof(TpPostingEntry);
		Size  new_size	   = new_capacity * sizeof(TpPostingEntry);

		/* Allocate new array with memory tracking */
		new_entries_dp = tp_dsa_allocate(
				local_state->dsa,
				&local_state->shared->memory_usage,
				new_size);
		if (!DsaPointerIsValid(new_entries_dp))
		{
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("pg_textsearch index memory limit exceeded"),
					 errdetail(
							 "Current usage: %zu bytes, limit: %zu bytes",
							 tp_get_memory_usage(
									 &local_state->shared->memory_usage),
							 tp_get_memory_limit()),
					 errhint("Increase pg_textsearch.index_memory_limit or "
							 "reduce the amount of data being indexed.")));
		}

		/* Copy existing entries if any */
		if (posting_list->doc_count > 0 &&
			DsaPointerIsValid(posting_list->entries_dp))
		{
			TpPostingEntry *old_entries =
					tp_get_posting_entries(local_state->dsa, posting_list);
			TpPostingEntry *new_entries =
					dsa_get_address(local_state->dsa, new_entries_dp);
			memcpy(new_entries,
				   old_entries,
				   posting_list->doc_count * sizeof(TpPostingEntry));

			/* Free old array with memory tracking */
			tp_dsa_free(
					local_state->dsa,
					&local_state->shared->memory_usage,
					posting_list->entries_dp,
					old_size);
		}

		posting_list->entries_dp = new_entries_dp;
		posting_list->capacity	 = new_capacity;
	}

	/* Add new document entry */
	entries			= tp_get_posting_entries(local_state->dsa, posting_list);
	new_entry		= &entries[posting_list->doc_count];
	new_entry->ctid = *ctid;
	new_entry->frequency = frequency;

	posting_list->doc_count++;
	posting_list->doc_freq	= posting_list->doc_count;
	posting_list->is_sorted = false; /* New entry may break sort order */
}

/*
 * Hash function for document length entries (CTID-based)
 */
static dshash_hash
tp_doclength_hash_function(const void *key, size_t keysize, void *arg)
{
	const ItemPointer ctid = (const ItemPointer)key;

	Assert(keysize == sizeof(ItemPointerData));
	(void)arg;

	/* Hash both block number and offset */
	return hash_bytes((const unsigned char *)ctid, sizeof(ItemPointerData));
}

/*
 * Compare function for document length entries (CTID comparison)
 */
static int
tp_doclength_compare_function(
		const void *a, const void *b, size_t keysize, void *arg)
{
	const ItemPointer ctid_a = (const ItemPointer)a;
	const ItemPointer ctid_b = (const ItemPointer)b;

	Assert(keysize == sizeof(ItemPointerData));
	(void)arg;

	return ItemPointerCompare(ctid_a, ctid_b);
}

/*
 * Copy function for document length entries (CTID copy)
 */
static void
tp_doclength_copy_function(
		void *dest, const void *src, size_t keysize, void *arg)
{
	Assert(keysize == sizeof(ItemPointerData));
	(void)arg;

	*((ItemPointer)dest) = *((const ItemPointer)src);
}

/*
 * Create document length hash table
 */
static dshash_table *
tp_doclength_table_create(dsa_area *area)
{
	dshash_parameters params;

	params.key_size			= sizeof(ItemPointerData);
	params.entry_size		= sizeof(TpDocLengthEntry);
	params.hash_function	= tp_doclength_hash_function;
	params.compare_function = tp_doclength_compare_function;
	params.copy_function	= tp_doclength_copy_function;
	params.tranche_id		= TP_DOCLENGTH_HASH_TRANCHE_ID;

	return dshash_create(area, &params, area);
}

/*
 * Attach to existing document length hash table
 */
dshash_table *
tp_doclength_table_attach(dsa_area *area, dshash_table_handle handle)
{
	dshash_parameters params;

	params.key_size			= sizeof(ItemPointerData);
	params.entry_size		= sizeof(TpDocLengthEntry);
	params.hash_function	= tp_doclength_hash_function;
	params.compare_function = tp_doclength_compare_function;
	params.copy_function	= tp_doclength_copy_function;
	params.tranche_id		= TP_DOCLENGTH_HASH_TRANCHE_ID;

	return dshash_attach(area, &params, handle, area);
}

/*
 * Store document length in the document length hash table
 */
void
tp_store_document_length(
		TpLocalIndexState *local_state, ItemPointer ctid, int32 doc_length)
{
	TpMemtable		 *memtable;
	dshash_table	 *doclength_table;
	TpDocLengthEntry *entry;
	bool			  found;

	Assert(local_state != NULL);
	Assert(ctid != NULL);

	memtable = get_memtable(local_state);
	if (!memtable)
		elog(ERROR, "Cannot get memtable - index state corrupted");

	/* Initialize document length table if needed */
	if (memtable->doc_lengths_handle == DSHASH_HANDLE_INVALID)
	{
		doclength_table = tp_doclength_table_create(local_state->dsa);
		memtable->doc_lengths_handle = dshash_get_hash_table_handle(
				doclength_table);
	}
	else
	{
		doclength_table = tp_doclength_table_attach(
				local_state->dsa, memtable->doc_lengths_handle);
	}

	/* Insert or update the document length */
	entry = (TpDocLengthEntry *)
			dshash_find_or_insert(doclength_table, ctid, &found);
	entry->ctid		  = *ctid;
	entry->doc_length = doc_length;

	dshash_release_lock(doclength_table, entry);
	dshash_detach(doclength_table);
}

/*
 * Get document length from the document length hash table
 */
int32
tp_get_document_length(TpLocalIndexState *local_state, ItemPointer ctid)
{
	TpMemtable		 *memtable;
	dshash_table	 *doclength_table;
	TpDocLengthEntry *entry;

	Assert(local_state != NULL);
	Assert(ctid != NULL);

	memtable = get_memtable(local_state);
	if (!memtable)
		elog(ERROR, "Cannot get memtable - index state corrupted");

	/* Check if document length table exists */
	if (memtable->doc_lengths_handle == DSHASH_HANDLE_INVALID)
		elog(ERROR,
			 "Document length table not initialized for CTID (%u,%u)",
			 BlockIdGetBlockNumber(&ctid->ip_blkid),
			 ctid->ip_posid);

	/* Attach to document length table */
	doclength_table = tp_doclength_table_attach(
			local_state->dsa, memtable->doc_lengths_handle);

	/* Look up the document length */
	entry = (TpDocLengthEntry *)dshash_find(doclength_table, ctid, false);
	if (entry)
	{
		int32 doc_length = entry->doc_length;
		dshash_release_lock(doclength_table, entry);
		dshash_detach(doclength_table);
		return doc_length;
	}
	else
	{
		dshash_detach(doclength_table);
		elog(ERROR,
			 "Document length not found for CTID (%u,%u)",
			 BlockIdGetBlockNumber(&ctid->ip_blkid),
			 ctid->ip_posid);
		return 0;
	}
}

/*
 * Shared memory cleanup - simplified stub
 */
