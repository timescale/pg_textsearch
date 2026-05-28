/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * cache.h — apply protocol for the in-memory memtable cache.
 *
 * The cache is derived state layered on top of the on-disk
 * memtable page chain (the source of truth).  This module owns
 * the two paths that mutate the cache from chain records:
 *
 *   tp_cache_cold_build()    bootstraps an empty cache from the
 *                            chain head.  Holds cache.apply_lock
 *                            EXCL + cache.lock EXCL (because it
 *                            allocates the dshash tables).
 *
 *   tp_cache_apply_to_tail() advances the cache cursor to the
 *                            current logical tail.  Holds
 *                            cache.apply_lock EXCL + cache.lock
 *                            SHARED, so concurrent readers can
 *                            keep serving from the cache while
 *                            catchup runs.
 *
 * Spill detection: every apply call generation-checks the
 * cursor's captured spill_generation against
 * TpSharedIndexState.spill_generation; on mismatch the cache is
 * dropped (apply_to_tail) or the build is rejected (cold_build's
 * RETRY).
 *
 * Lock order (see docs/memtable_cache.md "Lock order"):
 *   per-index LWLock (SHARED for read, EXCL for spill)
 *     -> cache.apply_lock (EXCL only)
 *       -> cache.lock (SHARED for readers, EXCL for drop/build)
 *         -> dshash buckets
 *           -> TpPostingList.lock
 *
 * Both entry points assume the caller already holds the
 * per-index LWLock at SHARED or stronger; they acquire and
 * release everything below it themselves.
 */
#pragma once

#include <postgres.h>

#include "index/state.h"
#include "utils/rel.h"

/*
 * Outcome of tp_cache_apply_to_tail().
 *
 *   OK              cursor advanced to the current logical
 *                   tail; all locks released.  Caller can serve
 *                   from the cache.
 *
 *   DROPPED         the cursor's captured spill_generation
 *                   disagreed with TpSharedIndexState; the
 *                   cache has been cleared and locks released.
 *                   Caller (read path) decides whether to
 *                   cold_build or fall back to chain_source.
 *
 *   BUDGET_EXCEEDED applying the next chain record would push
 *                   the cache footprint over the per-index soft
 *                   cap.  The cursor stays at the pre-record
 *                   position (so the next catchup attempt
 *                   resumes from the same point if eviction has
 *                   reclaimed budget); locks are released.
 *                   Caller falls back to chain_source.
 *
 *   NOT_INITIALIZED the cache has never been cold-built (or has
 *                   been cleared); cursor.next_blkno is
 *                   InvalidBlockNumber.  Caller must invoke
 *                   tp_cache_cold_build() first.
 */
typedef enum TpCacheApplyResult
{
	TP_CACHE_APPLY_OK,
	TP_CACHE_APPLY_DROPPED,
	TP_CACHE_APPLY_BUDGET_EXCEEDED,
	TP_CACHE_APPLY_NOT_INITIALIZED,
} TpCacheApplyResult;

/*
 * Outcome of tp_cache_cold_build().
 *
 *   OK    the cache was bootstrapped from the chain head; all
 *         locks released.  Caller can serve from the cache.
 *
 *   RETRY the cache was already populated when we acquired
 *         cache.lock EXCL (another backend cold-built while we
 *         were waiting).  Locks released.  Caller should
 *         re-attempt tp_cache_apply_to_tail.
 *
 *   ABORT the build was aborted because applying a record would
 *         push the cache footprint over the per-index soft cap.
 *         Partial state has been freed (tp_cache_clear), the
 *         cursor is back at (Invalid, 0), locks released.
 *         Caller falls back to chain_source.
 */
typedef enum TpCacheColdBuildResult
{
	TP_CACHE_COLD_OK,
	TP_CACHE_COLD_RETRY,
	TP_CACHE_COLD_ABORT,
} TpCacheColdBuildResult;

extern TpCacheApplyResult
tp_cache_apply_to_tail(TpLocalIndexState *local_state, Relation rel);

extern TpCacheColdBuildResult
tp_cache_cold_build(TpLocalIndexState *local_state, Relation rel);

/*
 * Per-index soft memory cap, in BYTES (the GUC is in kB).
 *
 * Returns the threshold that the apply paths compare
 * estimated_bytes against on every record.  0 means unlimited.
 *
 * Exposed for the SQL test scaffold and (in future phases) the
 * eviction policy; production callers should not gate on this
 * directly.
 */
extern uint64 tp_cache_per_index_soft_cap_bytes(void);
