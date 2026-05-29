/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * log.h - On-disk memtable write path.
 *
 * Appends document records to the on-disk memtable page chain
 * rooted at the metapage's (memtable_head_blkno,
 * memtable_tail_blkno) fields.  See issue #374 and
 * docs/memtable_v2.md for the design.
 *
 * Concurrency contract:
 *
 *     per-index LWLock SHARED/EXCL
 *         └─► tail buffer EXCL
 *               └─► new buffer EXCL (just-extended)
 *                     └─► metapage buffer EXCL
 *
 * Reverse-order acquisitions are forbidden.  Readers
 * (chain_source.c) take the metapage SHARED, read
 * memtable_head_blkno, and release the metapage before acquiring
 * any chain-page lock.  Spill (build.c) holds the per-index
 * LWLock EXCLUSIVE, which excludes all writers above; that is
 * the path by which spill gains uncontended access to the chain
 * pages.
 *
 * Crash safety: a crash between page allocation
 * (ExtendBufferedRel or FSM reuse via GetFreeIndexPage) and
 * GenericXLogFinish() leaves a block on disk that is unreachable
 * via meta.head → next chain and usually without a DEAD stamp.
 * No code may sequentially scan the relation without first
 * validating each page via tp_memtable_page_is_valid().  Orphans
 * without DEAD may be recycled later by amvacuumcleanup once
 * horizon rules exist for never-linked blocks.
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>
#include <utils/rel.h>

/*
 * Append one document record to the on-disk memtable chain.
 *
 * Caller responsibilities:
 *   - `rel` is open with at least RowExclusiveLock (heavyweight)
 *     and writes to it are permitted in the current xact
 *   - `vector_bytes` points to `vector_len` bytes of opaque
 *     payload (the v2 TpVector wire format; scaffold callers
 *     in log.c may pass synthetic bytes for unit-test coverage)
 *
 * On entry, this function:
 *   - Acquires the per-index LWLock in SHARED mode (idempotent
 *     within an xact via tp_acquire_index_lock).
 *   - Bootstraps the chain (first append) under one GenericXLog
 *     record covering {metapage, new page}; OR
 *   - Appends to the existing tail page under one GenericXLog
 *     record covering {tail}; OR
 *   - Extends the chain under one GenericXLog record covering
 *     {tail, new page, metapage}, then appends to new tail; OR
 *   - For oversized payloads (vector_len greater than
 *     TP_MEMTABLE_PAGE_MAX_VECTOR_LEN): writes a head fragment
 *     record on a fresh page followed by N continuation pages
 *     and a fresh tail; each new page gets its own GenericXLog
 *     record, and a final atomic GenericXLog publishes the
 *     pages by linking the old tail (or meta head) to the head
 *     fragment and advancing the meta tail.
 *
 * Returns the block number of the page that received the new
 * record.  The per-index LWLock is left held in SHARED mode on
 * return; the AM-level caller (`tp_insert`) releases it before
 * the auto-spill check, and the standard xact-end callback is a
 * safety net for any path that forgets to release.
 */
extern BlockNumber tp_memtable_append(
		Relation	rel,
		ItemPointer ctid,
		int32		doc_length,
		const char *vector_bytes,
		uint32		vector_len);

/*
 * Atomically publish a spill.  Used by the spill path after
 * tp_write_segment has produced a new L0 segment from the chain.
 *
 * Performs, in a single GenericXLog record:
 *   - Resets memtable_head_blkno / memtable_tail_blkno to
 *     InvalidBlockNumber on the metapage.
 *   - Splices the new segment in at the head of the L0 chain,
 *     setting its `next_segment` to the previous L0 head and
 *     bumping the metapage's level_heads[0] / level_counts[0].
 *   - Adds `docs_delta` / `len_delta` to total_docs / total_len
 *     (these are caller-supplied: the chain source's totals).
 *
 * Does not modify the old memtable chain pages.  After this call
 * the chain blocks are orphans (no metapage pointer); the caller
 * should run `tp_memtable_mark_chain_dead` to WAL-stamp each page
 * `DEAD` + `dead_fxid` for later reclaim (crash-safe ordering:
 * finalize first, then mark dead).  In-flight scans walking the
 * old chain via a metapage snapshot they already latched complete
 * safely; new scans see the new metapage and skip the orphans.
 * `tp_vacuumcleanup` returns them to the index FSM via
 * `RecordFreeIndexPage`; `tp_memtable_alloc_page` reuses them.
 *
 * Caller MUST already hold the per-index LWLock in EXCLUSIVE mode.
 *
 * `state` is required when the in-memory memtable cache is active
 * (runtime mode); it provides the DSA attachment used to drop the
 * cache's dshash tables after the metapage is published, and the
 * pg_atomic counter that the cache's apply protocol uses as a
 * stale-detection token.  Callers that legitimately have no cache
 * (e.g. spill-from-build before the local state is wired up) may
 * pass NULL; in that case the spill_generation bump and the
 * tp_cache_clear call are both skipped (the build path constructs
 * an empty cache anyway).
 */
extern void tp_spill_finalize(
		TpLocalIndexState *state,
		Relation		   rel,
		BlockNumber		   new_segment_root,
		uint64			   docs_delta,
		uint64			   len_delta);

/*
 * WAL-stamp every page in the memtable chain rooted at `head` as
 * dead (including fragment continuation pages).  Called from
 * `tp_do_spill` under LW_EXCLUSIVE after tp_spill_finalize has
 * disconnected the chain (crash-safe ordering: finalize first,
 * then mark dead).  `horizon` is stored in each page's `dead_fxid`
 * for later reclaim once globally safe.
 */
extern void tp_memtable_mark_chain_dead(
		Relation rel, BlockNumber head, FullTransactionId horizon);

/* Forward declaration to avoid heavy header include. */
typedef struct TpLocalIndexState TpLocalIndexState;

/*
 * Append one document to the on-disk memtable chain and update
 * the per-transaction bulk-load counter.
 *
 * Memtable v2 (issue #374): the previous DSA-based posting and
 * doc-length tables are gone; this is a thin wrapper around
 * tp_memtable_append() that also bumps terms_added_this_xact,
 * used by tp_bulk_load_spill_check.
 *
 * `vector_bytes` points to `vector_len` bytes of opaque payload
 * (the in-memory v2 TpVector wire format).  The chain source
 * reconstructs per-term postings on read.
 */
extern void tp_add_document_terms(
		TpLocalIndexState *local_state,
		Relation		   rel,
		ItemPointer		   ctid,
		const char		  *vector_bytes,
		uint32			   vector_len,
		int				   term_count,
		int32			   doc_length);
