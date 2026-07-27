/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * freepage.c - Recyclable free-page stamping for safe FSM reuse.
 *
 * See freepage.h for the rationale (issues #380, #426, #427).
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>

#include "constants.h"
#include "index/freepage.h"

bool
tp_page_is_recyclable(Page page)
{
	TpFreePageData *f = (TpFreePageData *)PageGetContents(page);

	return f->magic == TP_FREE_PAGE_MAGIC;
}

void
tp_record_free_index_page(Relation index, BlockNumber blk)
{
	Buffer			  buf;
	Page			  page;
	PageHeader		  ph;
	GenericXLogState *state;
	TpFreePageData	 *f;

	if (blk == TP_METAPAGE_BLKNO)
		elog(ERROR, "pg_textsearch: refusing to free metapage (block 0)");

	buf = ReadBuffer(index, blk);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	state = GenericXLogStart(index);
	page  = GenericXLogRegisterBuffer(state, buf, 0);

	f			  = (TpFreePageData *)PageGetContents(page);
	f->magic	  = TP_FREE_PAGE_MAGIC;
	f->flags	  = 0;
	f->freed_fxid = ReadNextFullTransactionId();

	/*
	 * Collapse the GenericXLog page hole so the stamp above lands in
	 * the WAL-logged lower region: computeDelta() diffs only
	 * [0, pd_lower) and [pd_upper, BLCKSZ), ignoring the hole in
	 * between.  Setting pd_lower = pd_upper = pd_special = BLCKSZ
	 * records the whole page while leaving the body bytes untouched,
	 * so the delta is just the header + stamp (a few dozen bytes) even
	 * though a merge/vacuum may free many pages.  Same page-hole
	 * convention as tp_tombstone_page_init.
	 */
	ph			   = (PageHeader)page;
	ph->pd_lower   = BLCKSZ;
	ph->pd_upper   = BLCKSZ;
	ph->pd_special = BLCKSZ;

	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);

	RecordFreeIndexPage(index, blk);
}

Buffer
tp_fsm_claim_free_buffer(Relation index)
{
	for (;;)
	{
		BlockNumber blk;
		Buffer		buf;
		Page		page;

		CHECK_FOR_INTERRUPTS();

		blk = GetFreeIndexPage(index);
		if (blk == InvalidBlockNumber)
			return InvalidBuffer; /* FSM empty: caller extends */

		/*
		 * Drop obviously bogus FSM entries.  GetFreeIndexPage() has
		 * already marked `blk` used, so simply skipping it removes the
		 * entry from circulation (no infinite loop, no re-offer).
		 */
		if (blk == TP_METAPAGE_BLKNO ||
			blk >= RelationGetNumberOfBlocks(index))
			continue;

		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		if (tp_page_is_recyclable(page))
			return buf; /* caller reinitializes + WAL-logs under the lock */

		/*
		 * The FSM offered a block that is NOT a deliberately-freed
		 * page — a stale (crash) or double-allocated (non-atomic
		 * GetFreeIndexPage) entry that still points at a live
		 * structure page.  Reusing it would corrupt that structure
		 * (issues #426, #427).  GetFreeIndexPage already cleared the
		 * FSM slot, so release the page and try the next candidate.
		 */
		UnlockReleaseBuffer(buf);
	}
}

BlockNumber
tp_fsm_claim_free_block(Relation index)
{
	Buffer		buf = tp_fsm_claim_free_buffer(index);
	BlockNumber blk;

	if (!BufferIsValid(buf))
		return InvalidBlockNumber;

	blk = BufferGetBlockNumber(buf);
	UnlockReleaseBuffer(buf);
	return blk;
}
