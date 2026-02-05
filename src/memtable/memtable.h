/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable.h - In-memory inverted index structures
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
#include "posting.h"
#include "state/state.h"

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

/* Default hash table size */
#define TP_DEFAULT_HASH_BUCKETS 1024

/* Function declarations */

/* Memtable-coordinated posting list access */
extern TpPostingList *
tp_get_posting_list(TpLocalIndexState *local_state, const char *term);
extern TpPostingList *tp_get_or_create_posting_list(
		TpLocalIndexState *local_state, const char *term);
