/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * freepage.h - Recyclable free-page stamping for FSM reuse.
 *
 * The index FSM (free space map) is a non-crash-safe hint whose
 * GetFreeIndexPage() claim is not atomic across backends.  Either
 * property can offer a block that is still referenced by a live
 * pg_textsearch structure — the on-disk memtable chain (issue #426),
 * the deferred-free tombstone chain (issue #380 / #427), a segment,
 * or a page index.  Blindly reinitializing such a block corrupts the
 * structure that still owns it.
 *
 * To make FSM reuse safe, every page returned to the FSM is first
 * WAL-stamped with TP_FREE_PAGE_MAGIC (tp_record_free_index_page).
 * Allocators then reuse a block ONLY when it still carries that stamp
 * (tp_fsm_claim_free_buffer / tp_fsm_claim_free_block).  A block the
 * FSM offers that is NOT stamped (a stale or double-allocated entry
 * pointing at a live page) is skipped rather than overwritten — the
 * same recyclability discipline PostgreSQL's B-tree uses via
 * _bt_page_recyclable.
 */
#pragma once

#include <postgres.h>

#include <access/transam.h>
#include <storage/block.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <utils/rel.h>

/*
 * Free-page stamp, written at PageGetContents() of a page that has
 * been deliberately returned to the index FSM.  The rest of the page
 * body is left intact (a reuse fully reinitializes it), keeping the
 * WAL delta tiny.
 */
typedef struct TpFreePageData
{
	uint32			  magic;	  /* TP_FREE_PAGE_MAGIC */
	uint32			  flags;	  /* reserved, 0 */
	FullTransactionId freed_fxid; /* xid at free time (diagnostic) */
} TpFreePageData;

/* True iff `page` carries the recyclable free-page stamp. */
extern bool tp_page_is_recyclable(Page page);

/*
 * WAL-stamp `blk` as a recyclable free page, then return it to the
 * index FSM.  Use in place of a bare RecordFreeIndexPage() so a later
 * allocator can distinguish a deliberately-freed page from a live one
 * whose block the FSM offered by mistake.  Takes the buffer's
 * EXCLUSIVE lock; the caller must already have removed `blk` from
 * every live structure (it is unreferenced once freed).
 */
extern void tp_record_free_index_page(Relation index, BlockNumber blk);

/*
 * Claim a recyclable free page from the FSM.  Returns a pinned,
 * EXCLUSIVE-locked buffer whose page the caller must reinitialize and
 * WAL-log before releasing the lock, or InvalidBuffer when the FSM
 * offers no reusable page (the caller then extends the relation).
 *
 * Holding the buffer lock from the recyclability check through the
 * caller's reinitialization makes the claim atomic: a second backend
 * handed the same block by a non-atomic GetFreeIndexPage() blocks on
 * the lock, then observes the now-live page and skips it.  This is the
 * path memtable inserts use (they run under the per-index lock only in
 * SHARED mode, so they race each other — issue #426).
 */
extern Buffer tp_fsm_claim_free_buffer(Relation index);

/*
 * Block-returning wrapper over tp_fsm_claim_free_buffer for callers
 * that reopen the block later under their own lock (the segment
 * writer buffers pages and flushes them afterward).  Because the lock
 * is dropped before the block is returned, this variant clears the
 * free stamp under the claim lock so the claim stays atomic even when
 * the caller holds the per-index lock only in SHARED mode and races a
 * concurrent allocator (e.g. VACUUM's pre-V5 segment rebuild vs. a
 * memtable insert).  Returns InvalidBlockNumber when the FSM offers no
 * reusable page.
 */
extern BlockNumber tp_fsm_claim_free_block(Relation index);
