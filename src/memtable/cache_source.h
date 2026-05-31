/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * cache_source.h — TpDataSource over the in-memory memtable
 *                  cache, plus the read-path chooser.
 *
 * The cache source serves get_postings / get_doc_length out of
 * the per-index dshash tables that the apply protocol (cache.c)
 * keeps in sync with the on-disk chain.  It is API-compatible
 * with `tp_memtable_chain_source_create` so call sites can swap
 * one for the other transparently.
 *
 * Concurrency contract (mirrors chain_source where possible):
 *
 *     per-index LWLock SHARED              (lifetime)
 *         └─► cache.apply_lock EXCL        (held only during apply/cold)
 *             └─► cache.lock SHARED         (lifetime, held by served source)
 *                 └─► dshash bucket lock    (per-lookup)
 *                     └─► TpPostingList.lock (per-lookup)
 *
 * The cache source holds the per-index LWLock and cache.lock
 * SHARED for its entire lifetime so concurrent spills (which
 * require both at EXCL) are excluded.  Apply/cold_build are
 * invoked once at construction with their preconditions
 * satisfied; the dshash attachments are then kept open until
 * close().
 *
 * Memory ownership: the source struct is palloc'd in
 * CurrentMemoryContext.  No private MemoryContext is created —
 * get_postings copies entries out into its own palloc'd
 * TpPostingData, which the caller frees via free_postings().
 */
#pragma once

#include <postgres.h>

#include <utils/rel.h>

#include "index/source.h"
#include "index/state.h"

/*
 * Construct a cache-backed data source for `rel`.
 *
 * On success the returned source holds:
 *   - the per-index LWLock at SHARED (acquired here if not already held)
 *   - cache.lock at SHARED (acquired here, released in close)
 *   - dshash attachments for the string and doc-length tables
 *
 * Returns NULL when the cache cannot serve the query — either
 * because the GUC is disabled, the per-index cache state is
 * unavailable, the apply protocol returned BUDGET_EXCEEDED, or
 * a second cold_build/apply attempt still failed.  Callers that
 * need a guaranteed source should use
 * tp_memtable_source_create_for_read() which falls back to the
 * chain source on NULL.
 *
 * `query_terms` and `query_term_count` are accepted for API
 * symmetry with the chain source but are currently ignored: the
 * cache already holds every term, so there is no per-query HTAB
 * pre-population analog.
 *
 * Caller must free via tp_source_close().
 */
extern TpDataSource *tp_memtable_cache_source_create(
		TpLocalIndexState *state,
		Relation		   rel,
		const char *const *query_terms,
		int				   query_term_count);

/*
 * Read-path chooser.
 *
 * Returns a TpDataSource that the caller can use without caring
 * whether the cache or the chain is the backing store.  When the
 * cache GUC is on and the cache successfully catches up to the
 * chain tail, returns a cache source; otherwise falls through to
 * tp_memtable_chain_source_create().  Returns NULL only when the
 * chain itself is empty (matching chain_source's empty-chain
 * NULL contract).
 *
 * Callers should treat the returned source identically to a
 * chain source: total_docs / total_len are populated, the
 * per-index LWLock is held SHARED for the source lifetime,
 * close() releases any locks acquired on behalf of the caller.
 *
 * Use this from scoring / standalone query paths.  Do NOT use it
 * from the spill path, debug dumps, or anything that needs the
 * chain accumulators directly (those continue to call
 * tp_memtable_chain_source_create explicitly).
 */
extern TpDataSource *tp_memtable_source_create_for_read(
		TpLocalIndexState *state,
		Relation		   rel,
		const char *const *query_terms,
		int				   query_term_count);
