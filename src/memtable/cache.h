/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * cache.h — lifecycle + apply protocol for the in-memory memtable cache.
 *
 * The cache is derived state layered on top of the on-disk
 * memtable page chain (the source of truth).  This module owns
 * three entry points: two that mutate the cache from chain
 * records, plus a teardown path.
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
 *   tp_cache_clear()         drops the dshash tables, resets the
 *                            cursor + estimated_bytes, and drains
 *                            accounting in lockstep with the
 *                            global counter.  Called on
 *                            generation-mismatch drop, spill
 *                            finalize, eviction, and index
 *                            teardown.  See the per-function
 *                            comment below for its lock contract.
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
 * Exposed for the SQL test scaffold and the eviction policy;
 * production callers should not gate on this directly.
 */
extern uint64 tp_cache_per_index_soft_cap_bytes(void);

/*
 * Global soft and hard memory caps, in BYTES (the GUC is in kB).
 * Global soft cap = memory_limit / 2; hard cap = memory_limit.
 * Both return 0 to mean "unlimited" when the GUC is disabled.
 * See docs/memtable_cache.md §"Memory cap (3 tiers)".
 */
extern uint64 tp_cache_global_soft_cap_bytes(void);
extern uint64 tp_cache_global_hard_cap_bytes(void);

/*
 * Outcome of tp_cache_evict_largest().
 *
 *   EVICTED        a victim was found and its cache cleared; the
 *                  drained bytes were subtracted from the global
 *                  counter.  Caller may re-check the global cap.
 *
 *   NOTHING_FOUND  no eligible victim (no registered index other
 *                  than the caller's, or the global counter
 *                  dropped under the threshold between argmax and
 *                  re-check).  Caller falls back to chain_source.
 *
 *   BUSY           the victim's per-index LWLock could not be
 *                  conditionally acquired (a reader is in flight).
 *                  Caller falls back to chain_source; a later
 *                  call may succeed.  Returning BUSY rather than
 *                  blocking is mandatory to avoid the
 *                  eviction-vs-reader deadlock documented in
 *                  cache.c (caller holds its own per-index
 *                  SHARED; blocking on the victim's per-index
 *                  EXCL while a concurrent reader on the victim
 *                  has already entered evict_largest would form
 *                  a cycle).
 */
typedef enum TpCacheEvictResult
{
	TP_CACHE_EVICT_EVICTED,
	TP_CACHE_EVICT_NOTHING_FOUND,
	TP_CACHE_EVICT_BUSY,
} TpCacheEvictResult;

/*
 * Pick the largest cache other than `caller_oid` and evict it
 * (clear its dshash tables, subtract its bytes from the global
 * counter).  Caller MUST hold its own per-index LWLock SHARED
 * (the read path's natural state); MUST NOT hold any cache lock.
 * See docs/memtable_cache.md §"Memory cap (3 tiers)".
 */
extern TpCacheEvictResult tp_cache_evict_largest(Oid caller_oid);

/*
 * Drain `memtable->estimated_bytes` to zero and subtract the
 * drained amount from the global estimated_total_bytes counter.
 * Called by tp_cache_clear; exposed for the eviction path which
 * drains symmetrically.  Safe to call when the memtable holds no
 * bytes (no-op).
 */
extern void tp_cache_account_bytes_drain(TpMemtable *memtable);

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
 * Lock contract.  Callers that may race against readers
 * (cold_build retry, generation-mismatch drop, spill_finalize)
 * acquire cache.lock EXCL before calling.  The DROP-INDEX /
 * subxact-abort teardown path runs while no other backend can be
 * reading the cache (AccessExclusiveLock on the index, or
 * shared-state already unregistered); no cache.lock acquisition
 * is needed there.  See docs/memtable_cache.md.
 */
extern void tp_cache_clear(dsa_area *dsa, TpMemtable *memtable);
