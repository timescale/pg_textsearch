/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * posting.h - In-memory posting list structures for DSA shared memory
 */
#pragma once

#include <postgres.h>

#include <storage/lwlock.h>
#include <storage/spin.h>
#include <utils/dsa.h>
#include <utils/hsearch.h>

#include "posting_entry.h"
#include "state/state.h"

/*
 * Posting list for a single term
 * Uses dynamic arrays with O(1) amortized inserts during building,
 * then sorts once at finalization for optimal query performance
 */
typedef struct TpPostingList
{
	int32		doc_count;	/* Length of the entries array */
	int32		capacity;	/* Allocated array capacity */
	bool		is_sorted;	/* True after final sort for queries */
	int32		doc_freq;	/* Document frequency (for IDF calculation) */
	dsa_pointer entries_dp; /* DSA pointer to TpPostingEntry array */
} TpPostingList;

/* Array growth multiplier */
extern int tp_posting_list_growth_factor;

/* Posting list memory management */
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

/* Document length hash table operations */
extern void tp_store_document_length(
		TpLocalIndexState *local_state, ItemPointer ctid, int32 doc_length);

extern int32 tp_get_document_length(
		TpLocalIndexState *local_state, Relation index, ItemPointer ctid);

/*
 * Get document length using a pre-attached doclength table.
 * This avoids repeated dshash_attach/detach overhead when looking up
 * multiple document lengths.
 */
extern int32 tp_get_document_length_attached(
		dshash_table *doclength_table, ItemPointer ctid);

extern dshash_table *
tp_doclength_table_attach(dsa_area *area, dshash_table_handle handle);

/* Index building operations */
extern float4 tp_calculate_idf(int32 doc_freq, int32 total_docs);

/* Shared memory cleanup */
extern void tp_cleanup_index_shared_memory(Oid index_oid);
