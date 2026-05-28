/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * posting.h — in-memory posting list + doclength entry types
 *
 * The cache holds DSA-resident posting lists keyed by term, each
 * a sorted (after finalization) array of (ctid, frequency)
 * entries.  Mutated by the cache apply protocol from chain doc
 * records; queried by the cache TpDataSource.  The doclength
 * hash entry (TpDocLengthEntry) is colocated here because the
 * doclength table API + DSA storage are owned by this module.
 * See docs/memtable_cache.md.
 */
#pragma once

#include <postgres.h>

#include <storage/lwlock.h>
#include <storage/spin.h>
#include <utils/dsa.h>
#include <utils/hsearch.h>

#include "index/state.h"
#include "memtable/posting_entry.h"

/*
 * Posting list for a single term
 * Uses dynamic arrays with O(1) amortized inserts during building,
 * then sorts once at finalization for optimal query performance
 */
typedef struct TpPostingList
{
	LWLock		lock;		/* Per-posting-list concurrency */
	int32		doc_count;	/* Length of the entries array */
	int32		capacity;	/* Allocated array capacity */
	bool		is_sorted;	/* True after final sort for queries */
	int32		doc_freq;	/* Document frequency (for IDF calculation) */
	dsa_pointer entries_dp; /* DSA pointer to TpPostingEntry array */
} TpPostingList;

/*
 * Document length entry for the dshash hash table.
 * Maps document CTID to its length for BM25 calculations.
 */
typedef struct TpDocLengthEntry
{
	ItemPointerData ctid;		/* Key: Document heap tuple ID */
	int32			doc_length; /* Value: Document length (sum of term
								 * frequencies) */
} TpDocLengthEntry;

/* Array growth multiplier */
extern int tp_posting_list_growth_factor;

/* Posting list memory management */
extern dsa_pointer tp_alloc_posting_list(dsa_area *dsa);
extern void tp_free_posting_list(dsa_area *area, dsa_pointer posting_list_dp);
extern TpPostingEntry *
tp_get_posting_entries(dsa_area *area, TpPostingList *posting_list);

extern void tp_add_document_to_posting_list(
		TpLocalIndexState *local_state,
		TpPostingList	  *posting_list,
		ItemPointer		   ctid,
		int32			   frequency);

/* Document length hash table tranche ID */
#define TP_DOCLENGTH_HASH_TRANCHE_ID (LWTRANCHE_FIRST_USER_DEFINED + 1)

/*
 * Reserve a doc-length slot for `ctid`. Returns true if newly
 * inserted, false if an entry for `ctid` was already present.
 * Acts as the idempotency gate in tp_cache_apply_document so
 * that concurrent appliers cannot double-add the same CTID.
 */
extern bool tp_store_document_length(
		TpLocalIndexState *local_state, ItemPointer ctid, int32 doc_length);

/*
 * Get document length using a pre-attached doclength table.
 * This avoids repeated dshash_attach/detach overhead when looking up
 * multiple document lengths.
 */
extern int32 tp_get_document_length_attached(
		dshash_table *doclength_table, ItemPointer ctid);

extern dshash_table *tp_doclength_table_create(dsa_area *area);
extern dshash_table *
tp_doclength_table_attach(dsa_area *area, dshash_table_handle handle);

/* tp_calculate_idf lives in src/scoring/bm25.c; declared in
 * src/scoring/bm25.h.  Not re-declared here. */

/* tp_cleanup_index_shared_memory lives in src/index/state.c; declared
 * in src/index/state.h.  Not re-declared here. */
