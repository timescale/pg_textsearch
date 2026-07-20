/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * tombstone.h - Deferred-free tombstone chain for displaced
 * segment pages (issue #380).
 *
 * A tombstone page parks the block numbers of segment pages that a
 * merge displaced, together with the merge's transaction horizon.
 * The pages are returned to the index FSM only once the horizon is
 * past GetOldestNonRemovableTransactionId, which (with
 * hot_standby_feedback=on) is past every standby snapshot.  This
 * defers physical reuse until no standby query can still be reading
 * the old segment through those pages.
 */
#pragma once

#include <postgres.h>

#include <access/transam.h>
#include <storage/block.h>
#include <storage/bufpage.h>
#include <utils/rel.h>

#include "constants.h"

/*
 * On-disk tombstone page, stored at PageGetContents(page).  The
 * blocks[] array lives in the page body, so tp_tombstone_page_init
 * sets pd_lower = BLCKSZ (the GenericXLog page-hole guard).
 */
typedef struct TpTombstonePageData
{
	uint32			  magic;	   /* TP_TOMBSTONE_MAGIC */
	uint16			  version;	   /* TP_TOMBSTONE_VERSION */
	uint16			  flags;	   /* reserved, 0 */
	FullTransactionId merged_fxid; /* reclaim horizon for this page */
	BlockNumber		  next_page;   /* next tombstone, or Invalid */
	uint32			  num_blocks;  /* used entries in blocks[] */
	BlockNumber		  blocks[FLEXIBLE_ARRAY_MEMBER];
} TpTombstonePageData;

typedef TpTombstonePageData *TpTombstonePage;

static inline TpTombstonePage
tp_tombstone_page(Page page)
{
	return (TpTombstonePage)PageGetContents(page);
}

/* Max block entries that fit on one tombstone page. */
#define TP_TOMBSTONE_CAPACITY                            \
	((uint32)((BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
			   offsetof(TpTombstonePageData, blocks)) /  \
			  sizeof(BlockNumber)))

extern void tp_tombstone_page_init(
		Page page, FullTransactionId merged_fxid, BlockNumber next_page);
extern bool tp_tombstone_page_is_valid(Page page);

/*
 * Read metap->pending_free_head (SHARE-locked).  Returns
 * InvalidBlockNumber for v6/v7 metapages (the field predates v8).
 */
extern BlockNumber tp_tombstone_read_head(Relation index);

/*
 * Park `num_blocks` displaced blocks into one or more freshly
 * allocated tombstone pages, stamping `merged_fxid` and chaining the
 * batch tail to `old_head`.  Each page is written in its own
 * GenericXLog record (still unreferenced until the caller installs
 * the returned head into metap->pending_free_head in the SAME record
 * as the level-swap).  Returns the head block of the new batch, or
 * `old_head` unchanged when num_blocks == 0.
 */
extern BlockNumber tp_tombstone_enqueue(
		Relation		  index,
		BlockNumber		 *blocks,
		uint32			  num_blocks,
		FullTransactionId merged_fxid,
		BlockNumber		  old_head);

/*
 * Variant for callers that do not serialize against concurrent FSM
 * allocators.  Extends the relation instead of calling GetFreeIndexPage().
 */
extern BlockNumber tp_tombstone_enqueue_extend(
		Relation		  index,
		BlockNumber		 *blocks,
		uint32			  num_blocks,
		FullTransactionId merged_fxid,
		BlockNumber		  old_head);

/*
 * Drain past-horizon tombstones.  For each tombstone whose
 * merged_fxid < `horizon`, WAL-unlink it then RecordFreeIndexPage its
 * listed blocks and its own page.
 *
 * `own_lock` selects locking discipline:
 *   - true  (vacuum path): acquire/release the per-index LWLock
 *     EXCLUSIVE around each single unlink, so reads never wait more
 *     than one unlink.  `state` must be non-NULL.
 *   - false (merge path): caller already holds the per-index lock
 *     EXCLUSIVE end-to-end; `state` is ignored.
 *
 * Returns the number of index pages returned to the FSM (listed
 * blocks + tombstone pages).  Caller runs IndexFreeSpaceMapVacuum
 * when the return value is > 0.
 */
struct TpLocalIndexState;
extern uint32 tp_tombstone_drain(
		Relation				  index,
		struct TpLocalIndexState *state,
		FullTransactionId		  horizon,
		bool					  own_lock);

/*
 * Total displaced blocks currently parked (debug/observability).
 * Caller must hold the per-index LWLock in shared mode.
 */
extern uint64 tp_pending_free_block_count(Relation index);

/*
 * Highest (block + 1) referenced by the tombstone chain — both the
 * tombstone pages themselves and the displaced blocks they list.
 * Returns 0 when the chain is empty.  Index truncation folds this
 * into its high-water mark so parked-but-not-yet-freed pages are
 * never shrunk away (issue #380).
 */
extern BlockNumber tp_tombstone_max_used_block(Relation index);
