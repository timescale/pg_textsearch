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

#include <access/generic_xlog.h>
#include <access/transam.h>
#include <storage/block.h>
#include <storage/bufmgr.h>
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

typedef struct TpDetachedTombstoneBatch
{
	BlockNumber head;
	BlockNumber tail;
	/* Exact container allocations owned until publication succeeds. */
	BlockNumber *owned_pages;
	uint32		 owned_count;
} TpDetachedTombstoneBatch;

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
 * Build an unreachable batch of tombstone pages.  The tail initially
 * links to InvalidBlockNumber; neither the metapage nor an existing
 * tombstone page is changed.
 */
extern void tp_tombstone_build_detached(
		Relation				  index,
		const BlockNumber		 *blocks,
		uint32					  num_blocks,
		FullTransactionId		  merged_fxid,
		TpDetachedTombstoneBatch *batch);

/*
 * Replace the reclaim stamp on every tracked container page.  Ordinary
 * compaction calls this while the batch is detached; parallel VACUUM calls it
 * after the provisionally stamped batch becomes reachable.
 */
extern void tp_tombstone_restamp_batch(
		Relation				 index,
		TpDetachedTombstoneBatch batch,
		FullTransactionId		 merged_fxid);

/*
 * Register the detached tail in the caller's GenericXLog publication record
 * and link it to old_head.  A parallel replacement may allow an invalid
 * provisional stamp.  Returns the still-locked tail buffer, which the caller
 * must release after GenericXLogFinish.
 */
extern Buffer tp_tombstone_attach_detached(
		GenericXLogState		*state,
		Relation				 index,
		TpDetachedTombstoneBatch batch,
		BlockNumber				 old_head,
		bool					 allow_invalid_stamp);

/*
 * Return only an unreachable batch's container pages to the FSM.
 * The displaced source blocks listed in those pages remain untouched.
 */
extern void
tp_tombstone_discard_detached(Relation index, TpDetachedTombstoneBatch batch);

/*
 * Drain past-horizon tombstones.  For each tombstone whose
 * merged_fxid < `horizon`, WAL-unlink it then tp_record_free_index_page
 * its listed blocks and its own page.
 *
 * `own_lock` selects locking discipline:
 *   - true  (vacuum path): take the per-index LWLock EXCLUSIVE once
 *     per drained tombstone, held across the unlink and that
 *     tombstone's page frees (see tp_tombstone_drain).  Readers wait
 *     for one chain walk, unlink, and up to TP_TOMBSTONE_CAPACITY
 *     frees.  `state` must be non-NULL.
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
