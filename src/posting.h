/*
 * posting.h - Tapir In-Memory Posting Lists
 *
 * This module implements in-memory posting lists for Tapir indexes.  These
 * are used to buffer updates until the index is flushed to disk.
 */

#pragma once

#include <postgres.h>

#include <storage/itemptr.h>
#include <storage/lwlock.h>
#include <storage/spin.h>
#include <utils/hsearch.h>

#include "index.h"
#include "memtable.h"
#include "stringtable.h"

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

extern int32
tp_get_document_length(TpLocalIndexState *local_state, ItemPointer ctid);

/* Index building operations */
extern float4 tp_calculate_idf(int32 doc_freq, int32 total_docs);

/* Shared memory cleanup */
extern void tp_cleanup_index_shared_memory(Oid index_oid);
