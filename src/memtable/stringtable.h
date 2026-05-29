/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * stringtable.h — string interning hash table for the in-memory
 * memtable cache.  Maps interned terms to their DSA-resident
 * posting lists via dshash.  See docs/memtable_cache.md.
 */
#pragma once

#include <postgres.h>

#include <lib/dshash.h>
#include <storage/itemptr.h>
#include <storage/lwlock.h>
#include <utils/dsa.h>

#include "memtable/posting.h"

typedef struct TpStringHashEntry TpStringHashEntry;

/*
 * Key structure that supports both lookup via char* and storage via
 * dsa_pointer. posting_list is set to InvalidDsaPointer for lookup keys and
 * non-InvalidDsaPointer for table entry keys.  Tricky but avoids allocations
 * for lookups while saving space in the hash table.
 *
 * For lookup keys: term.str points to (possibly non-null-terminated) string,
 * len contains the string length.
 * For stored keys: term.dp points to null-terminated DSA string, len is
 * unused.
 */
typedef struct TpStringKey
{
	union
	{
		const char *str;
		dsa_pointer dp;
	} term;

	dsa_pointer posting_list;
	uint32		len; /* String length for lookup keys (avoids strlen) */
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
extern dshash_table *
tp_string_table_attach(dsa_area *area, dshash_table_handle handle);

/* Core hash table operations */
extern TpStringHashEntry *tp_string_table_lookup(
		dsa_area *area, dshash_table *ht, const char *str, size_t len);
extern void tp_string_table_clear(dsa_area *area, dshash_table *ht);

/* Ensure the string hash table is initialized (call under EXCLUSIVE) */
extern void tp_ensure_string_table_initialized(TpLocalIndexState *local_state);

/* Document term management functions
 *
 * tp_cache_apply_document is the cache-side analogue of v2's
 * existing tp_add_document_terms (src/memtable/log.c).  The
 * on-disk version appends a doc record to the on-disk chain
 * (source of truth).
 * This version applies an already-parsed record (terms +
 * frequencies + doc_length for a CTID) into the in-memory cache
 * structures (string interning table, posting lists, doclength
 * table).  Used by the cache apply protocol to bring the cache
 * up to date with the chain. */
extern void tp_cache_apply_document(
		TpLocalIndexState *local_state,
		ItemPointer		   ctid,
		char			 **terms,
		int32			  *frequencies,
		int				   term_count,
		int32			   doc_length);

/* LWLock tranche for string table locking */
#define TP_STRING_HASH_TRANCHE_ID LWTRANCHE_FIRST_USER_DEFINED
