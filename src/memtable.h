/*-------------------------------------------------------------------------
 *
 * memtable.h
 *	  Tapir in-memory table structures and function declarations
 *
 * IDENTIFICATION
 *	  src/memtable.h
 *
 *-------------------------------------------------------------------------
 */
#pragma once

#include <postgres.h>

#include <lib/dshash.h>
#include <storage/dsm_registry.h>
#include <storage/itemptr.h>
#include <storage/lwlock.h>
#include <storage/spin.h>
#include <utils/dsa.h>
#include <utils/hsearch.h>
#include <utils/timestamp.h>

#include "constants.h"
#include "index.h"

/*
 * Forward declarations to avoid circular dependencies
 */
typedef struct TpPostingEntry TpPostingEntry;
typedef struct TpPostingList  TpPostingList;

/*
 * Document length entry for the dshash hash table
 * Maps document CTID to its length for BM25 calculations
 */
typedef struct TpDocLengthEntry
{
	ItemPointerData ctid;		/* Key: Document heap tuple ID */
	int32			doc_length; /* Value: Document length (sum of term
								 * frequencies) */
} TpDocLengthEntry;

/*
 * Individual document occurrence within a posting list
 */
struct TpPostingEntry
{
	ItemPointerData ctid;	   /* Document heap tuple ID */
	int32			doc_id;	   /* Internal document ID */
	int32			frequency; /* Term frequency in document */
};

/*
 * Posting list for a single term
 * Uses dynamic arrays with O(1) amortized inserts during building,
 * then sorts once at finalization for optimal query performance
 */
struct TpPostingList
{
	int32		doc_count;	/* Length of the entries array */
	int32		capacity;	/* Allocated array capacity */
	bool		is_sorted;	/* True after final sort for queries */
	int32		doc_freq;	/* Document frequency (for IDF calculation) */
	dsa_pointer entries_dp; /* DSA pointer to TpPostingEntry array */
};

/* GUC variables */
extern int tp_index_memory_limit; /* Currently not enforced */

/* Default hash table size */
#define TP_DEFAULT_HASH_BUCKETS 1024

/* Function declarations */

/* Memtable-coordinated posting list access */
extern TpPostingList *
tp_get_posting_list(TpLocalIndexState *local_state, const char *term);
extern TpPostingList *tp_get_or_create_posting_list(
		TpLocalIndexState *local_state, const char *term);

/* DSA-based per-index management */
extern void tp_destroy_index_dsa(Oid index_oid);
