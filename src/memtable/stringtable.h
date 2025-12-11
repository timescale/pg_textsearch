/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */

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

#include <postgres.h>

#include <lib/dshash.h>
#include <storage/itemptr.h>
#include <storage/lwlock.h>
#include <utils/dsa.h>

typedef struct TpStringHashEntry TpStringHashEntry;
typedef struct TpPostingList	 TpPostingList;
typedef struct TpLocalIndexState TpLocalIndexState;
typedef struct TpPostingEntry	 TpPostingEntry;

/*
 * Key structure that supports both lookup via char* and storage via
 * dsa_pointer. posting_list is set to InvalidDsaPointer for lookup keys and
 * non-InvalidDsaPointer for table entry keys.  Tricky but voids allocations
 * for lookups while saving space in the hash table.
 */
typedef struct TpStringKey
{
	union
	{
		const char *str;
		dsa_pointer dp;
	} term;

	dsa_pointer posting_list;
} TpStringKey;

/* Helper function to get the string from a key */
extern const char *tp_get_key_str(dsa_area *area, const TpStringKey *key);

/*
 * dshash entry structure for string interning and posting list mapping
 * Key distinguishes between local char* (for lookups) and DSA strings (for
 * storage)
 */
struct TpStringHashEntry
{
	/* Term plus a pointer to the posting list */
	TpStringKey key;
};

/* String table creation and initialization */
extern dshash_table *tp_string_table_create(dsa_area *area);
extern dshash_table *
tp_string_table_attach(dsa_area *area, dshash_table_handle handle);

/* Core hash table operations */
extern TpStringHashEntry *tp_string_table_lookup(
		dsa_area *area, dshash_table *ht, const char *str, size_t len);
extern TpStringHashEntry *tp_string_table_insert(
		dsa_area *area, dshash_table *ht, const char *str, size_t len);
extern bool tp_string_table_delete(
		dsa_area *area, dshash_table *ht, const char *str, size_t len);
extern void tp_string_table_clear(dsa_area *area, dshash_table *ht);

/* Document term management functions */
extern void tp_add_document_terms(
		TpLocalIndexState *local_state,
		ItemPointer		   ctid,
		char			 **terms,
		int32			  *frequencies,
		int				   term_count,
		int32			   doc_length);

/* Pure string table operations (take dshash_table parameter) */
extern TpPostingList *tp_string_table_get_or_create_posting_list(
		dsa_area *area, dshash_table *ht, const char *term);

/* Posting list access via string table */
extern TpPostingList *tp_string_table_get_posting_list(
		dsa_area *area, dshash_table *ht, const char *term);

/* Posting list management in DSA */
extern dsa_pointer tp_alloc_posting_list(dsa_area *dsa);
extern TpPostingEntry *
tp_alloc_posting_entries_dsa(dsa_area *area, uint32 capacity);
extern TpPostingEntry *tp_realloc_posting_entries_dsa(
		dsa_area *area, TpPostingList *posting_list, uint32 new_capacity);

/* Helper functions for dsa_pointer conversion */
extern TpPostingList *
tp_get_posting_list_from_dp(dsa_area *area, dsa_pointer dp);
extern TpPostingEntry			   *
tp_get_posting_entries_from_dp(dsa_area *area, dsa_pointer dp);
extern char *tp_get_string_from_dp(dsa_area *area, dsa_pointer dp);

/* LWLock tranche for string table locking */
#define TP_STRING_HASH_TRANCHE_ID LWTRANCHE_FIRST_USER_DEFINED
