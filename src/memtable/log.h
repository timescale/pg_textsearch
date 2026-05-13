/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * log.h - On-disk memtable write path.
 *
 * Phase 2 of the memtable v2 redesign (see issue #374 and
 * plan.md).  This module appends document records to the
 * on-disk memtable page chain rooted at the metapage's
 * (memtable_head_blkno, memtable_tail_blkno) fields.
 *
 * Concurrency contract (committed to in Phase 2, enforced by
 * later phases):
 *
 *     per-index LWLock SHARED/EXCL
 *         └─► tail buffer EXCL
 *               └─► new buffer EXCL (just-extended)
 *                     └─► metapage buffer EXCL
 *
 * Reverse-order acquisitions are forbidden.  Readers (Phase 3)
 * take the metapage SHARED, read memtable_head_blkno, and
 * release the metapage before acquiring any chain-page lock.
 * Spill (Phase 4) holds the per-index LWLock EXCLUSIVE, which
 * excludes all writers above; that is the path by which spill
 * gains uncontended access to the chain pages.
 *
 * Crash safety: a crash between ExtendBufferedRel() and
 * GenericXLogFinish() leaves an uninitialized block on disk
 * that is unreachable via meta.head → next chain.  No code may
 * sequentially scan the relation without first validating each
 * page via tp_memtable_page_is_valid().  See plan.md for
 * additional discussion.
 *
 * The writer is not yet wired into the index insert path.  In
 * Phase 2 it is exposed only through the scaffold SQL functions
 * defined in log.c; the unified switchover happens in Phase 4.
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
 *     payload (in the final design, the v2 TpVector wire
 *     format; in Phase 2 scaffold callers may pass synthetic
 *     bytes)
 *
 * On entry, this function:
 *   - Validates `vector_len` against TP_MEMTABLE_PAGE_MAX_VECTOR_LEN
 *     and ereports ERROR if oversized.  No buffer or WAL work
 *     is performed in that case.
 *   - Acquires the per-index LWLock in SHARED mode (idempotent
 *     within an xact via tp_acquire_index_lock).
 *   - Bootstraps the chain (first append) under one GenericXLog
 *     record covering {metapage, new page}; OR
 *   - Appends to the existing tail page under one GenericXLog
 *     record covering {tail}; OR
 *   - Extends the chain under one GenericXLog record covering
 *     {tail, new page, metapage}, then appends to new tail.
 *
 * Returns the block number of the page that received the new
 * record.  The per-index LWLock remains held in SHARED mode for
 * the duration of the transaction (released by the standard
 * xact-end callback).
 */
extern BlockNumber tp_memtable_append(
		Relation	rel,
		ItemPointer ctid,
		int32		doc_length,
		const char *vector_bytes,
		uint32		vector_len);
