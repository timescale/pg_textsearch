/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * chain_source.h - TpDataSource over the on-disk memtable page
 *                  chain (Phase 3 of issue #374).
 *
 * Reads document records from the chain rooted at
 * meta.memtable_head_blkno (introduced in Phase 2), aggregates
 * them into in-memory per-term posting lists and a per-ctid
 * doc-length map, and serves them through the standard
 * TpDataSourceOps interface.
 *
 * Concurrency contract:
 *
 *     per-index LWLock SHARED
 *         └─► metapage buffer SHARED  (held briefly, released)
 *         └─► chain page buffer SHARED  (one at a time)
 *
 * The per-index LWLock SHARED acquisition (idempotent within an
 * xact via tp_acquire_index_lock) excludes spill (which holds
 * LW_EXCLUSIVE) from racing the eager open-walk.  Concurrent
 * writers also hold the per-index lock SHARED so they coexist
 * with us; their tail-buffer EXCL serializes against our SHARED
 * read of the same page.
 *
 * Memory ownership: the constructor creates a child
 * MemoryContext under CurrentMemoryContext; all accumulators,
 * key copies, and HTAB internals live inside it.  `close()`
 * deletes the child context in one shot (no per-entry frees).
 *
 * Phase 3 scope: this module is NOT yet wired into the index
 * AM.  The existing DSA-backed tp_memtable_source_create() is
 * still the active source; this code is exercised only through
 * the scaffold SQL function defined in chain_source.c.  The
 * unified switchover happens in Phase 4 together with spill.
 */
#pragma once

#include <postgres.h>

#include <utils/rel.h>

#include "index/source.h"
#include "index/state.h"

/*
 * Construct a chain-backed data source for `rel`.
 *
 * Eagerly walks the entire chain in the constructor (one pass,
 * one SHARED buffer lock at a time).  The returned TpDataSource
 * serves get_postings / get_doc_length from in-memory
 * accumulators thereafter.
 *
 * Returns NULL if the chain is empty (mirrors the existing
 * tp_memtable_source_create() contract that an absent memtable
 * is signalled by a NULL source).
 *
 * Caller must free via tp_source_close().
 */
extern TpDataSource *
tp_memtable_chain_source_create(TpLocalIndexState *state, Relation rel);
