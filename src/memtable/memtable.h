/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable.h — in-memory memtable cache structures
 *
 * Defines TpDocLengthEntry (doclength hash table entry) used by
 * the in-memory cache that sits in DSA on top of the on-disk
 * memtable page chain.  The cache is the in-memory analog of the
 * inverted index materialized once per chain generation.
 *
 * See docs/memtable_cache.md for the design.
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
#include "index/state.h"
#include "memtable/posting.h"

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

/*
 * Drop the in-memory cache state.  Frees the dshash inverted index
 * and doc-length table (returning their DSA memory to the arena),
 * resets the apply cursor and estimated_bytes to their post-init
 * values, and trims the DSA.  Safe to call when the cache is
 * already empty (handles == DSHASH_HANDLE_INVALID): a no-op then.
 *
 * Does NOT touch:
 *   - apply_lock / lock: these LWLocks live inside the TpMemtable
 *     struct and must survive across clears so subsequent apply
 *     paths reuse them.  The struct itself is freed only by the
 *     surrounding dsa_free on memtable_dp.
 *   - TpSharedIndexState.spill_generation: that's the spill path's
 *     invalidation token; tp_cache_clear is a consumer of it, not
 *     a writer.
 *   - corpus stats in TpSharedIndexState: those reflect the on-disk
 *     chain (source of truth), not the cache.
 *
 * Lock contract.  Phase 3+ callers (cold_build retry,
 * generation-mismatch drop, spill_finalize) acquire cache.lock EXCL
 * before calling.  Phase 2's only caller is the DROP-INDEX /
 * subxact-abort teardown path, which runs while no other backend
 * can be reading the cache (AccessExclusiveLock on the index, or
 * shared-state already unregistered); no cache.lock acquisition is
 * needed there.  See docs/memtable_cache.md.
 */
extern void tp_cache_clear(dsa_area *dsa, TpMemtable *memtable);
