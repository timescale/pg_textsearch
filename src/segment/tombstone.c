/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * tombstone.c - Deferred-free tombstone chain (issue #380).
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/freespace.h>
#include <storage/indexfsm.h>

#include "constants.h"
#include "index/metapage.h"
#include "index/state.h"
#include "segment/io.h"
#include "segment/tombstone.h"

void
tp_tombstone_page_init(
		Page page, FullTransactionId merged_fxid, BlockNumber next_page)
{
	TpTombstonePage t;

	PageInit(page, BLCKSZ, 0);

	t		   = tp_tombstone_page(page);
	t->magic	   = TP_TOMBSTONE_MAGIC;
	t->version	   = TP_TOMBSTONE_VERSION;
	t->flags	   = 0;
	t->merged_fxid = merged_fxid;
	t->next_page   = next_page;
	t->num_blocks  = 0;

	/*
	 * blocks[] lives in the page body; collapse the GenericXLog
	 * page hole so the array is recorded and not zeroed on replay.
	 * Same convention as the memtable page (src/memtable/page.c).
	 */
	((PageHeader)page)->pd_lower = BLCKSZ;
}

bool
tp_tombstone_page_is_valid(Page page)
{
	TpTombstonePage t = tp_tombstone_page(page);

	return t->magic == TP_TOMBSTONE_MAGIC &&
		   t->version == TP_TOMBSTONE_VERSION &&
		   t->num_blocks <= TP_TOMBSTONE_CAPACITY;
}

/*
 * Allocate one zero/extend index page via the FSM-or-extend path.
 * Mirrors allocate_segment_page() but is local to this module; the
 * page is fully overwritten by the GenericXLog image below, so its
 * pre-existing contents are irrelevant.
 */
static BlockNumber
tombstone_alloc_page(Relation index)
{
	Buffer		buffer;
	BlockNumber block;

	block = GetFreeIndexPage(index);
	if (block != InvalidBlockNumber)
		return block;

	buffer = ReadBufferExtended(
			index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
	block = BufferGetBlockNumber(buffer);
	UnlockReleaseBuffer(buffer);
	return block;
}

BlockNumber
tp_tombstone_enqueue(
		Relation		  index,
		BlockNumber		 *blocks,
		uint32			  num_blocks,
		FullTransactionId merged_fxid,
		BlockNumber		  old_head)
{
	BlockNumber batch_head = old_head;
	uint32		remaining = num_blocks;

	if (num_blocks == 0)
		return old_head;

	/*
	 * Build the batch tail-first so each page's next_page points at
	 * an already-decided successor: the first page we write links to
	 * old_head, and each subsequent page links to the previous one.
	 * batch_head ends up at the last page written.
	 *
	 * Per-page chunking honors TP_TOMBSTONE_CAPACITY.  We assign the
	 * LAST chunk of `blocks` to the first page, walking backwards.
	 */
	while (remaining > 0)
	{
		uint32			  chunk = Min(remaining, TP_TOMBSTONE_CAPACITY);
		uint32			  start = remaining - chunk;
		BlockNumber		  blk	= tombstone_alloc_page(index);
		GenericXLogState *state;
		Buffer			  buf;
		Page			  page;
		TpTombstonePage	  t;
		uint32			  k;

		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		state = GenericXLogStart(index);
		page  = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);

		tp_tombstone_page_init(page, merged_fxid, batch_head);
		t		  = tp_tombstone_page(page);
		t->num_blocks = chunk;
		for (k = 0; k < chunk; k++)
			t->blocks[k] = blocks[start + k];

		GenericXLogFinish(state);
		UnlockReleaseBuffer(buf);

		batch_head = blk;
		remaining  = start;
	}

	return batch_head;
}

/*
 * Read pending_free_head from the metapage under a SHARE lock.
 */
BlockNumber
tp_tombstone_read_head(Relation index)
{
	Buffer			buf;
	Page			page;
	TpIndexMetaPage metap;
	BlockNumber		head;

	buf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page  = BufferGetPage(buf);
	metap = (TpIndexMetaPage)PageGetContents(page);
	/* v6/v7 pages predate the field; treat as empty. */
	if (metap->version < TP_METAPAGE_VERSION)
		head = InvalidBlockNumber;
	else
		head = metap->pending_free_head;
	UnlockReleaseBuffer(buf);
	return head;
}

/*
 * WAL-unlink the tombstone at `victim` whose predecessor is
 * `prev` (InvalidBlockNumber => the metapage is the predecessor).
 * `victim_next` is the victim's next_page (the new link target).
 * Upgrades the metapage to v8 first when the predecessor is the
 * metapage (so the field exists to write).
 */
static void
tombstone_unlink(
		Relation	index,
		BlockNumber prev,
		BlockNumber victim,
		BlockNumber victim_next)
{
	GenericXLogState *state;
	Buffer			  buf;
	Page			  page;

	(void) victim;

	state = GenericXLogStart(index);

	if (prev == InvalidBlockNumber)
	{
		TpIndexMetaPage metap;

		buf	 = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = GenericXLogRegisterBuffer(state, buf, 0);
		tp_metapage_upgrade_to_current(index, page);
		metap					 = (TpIndexMetaPage)PageGetContents(page);
		metap->pending_free_head = victim_next;
	}
	else
	{
		TpTombstonePage t;

		buf	 = ReadBuffer(index, prev);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
		t		 = tp_tombstone_page(page);
		t->next_page = victim_next;
	}

	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
}

uint32
tp_tombstone_drain(
		Relation				  index,
		struct TpLocalIndexState *state,
		FullTransactionId		  horizon,
		bool					  own_lock)
{
	uint32		freed	= 0;
	BlockNumber nblocks = RelationGetNumberOfBlocks(index);

	for (;;)
	{
		BlockNumber	 prev		   = InvalidBlockNumber;
		BlockNumber	 cur;
		BlockNumber	 victim		   = InvalidBlockNumber;
		BlockNumber	 victim_prev   = InvalidBlockNumber;
		BlockNumber	 victim_next   = InvalidBlockNumber;
		BlockNumber *victim_blocks = NULL;
		uint32		 victim_count  = 0;

		CHECK_FOR_INTERRUPTS();

		if (own_lock)
			tp_acquire_index_lock(state, LW_EXCLUSIVE);

		/* Walk the chain to find the first past-horizon tombstone. */
		cur = tp_tombstone_read_head(index);
		while (cur != InvalidBlockNumber)
		{
			Buffer			buf;
			Page			page;
			TpTombstonePage t;

			buf = ReadBuffer(index, cur);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);

			if (!tp_tombstone_page_is_valid(page))
			{
				UnlockReleaseBuffer(buf);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("pg_textsearch: corrupt tombstone page %u "
								"in index \"%s\"",
								cur, RelationGetRelationName(index))));
			}

			t = tp_tombstone_page(page);
			if (FullTransactionIdPrecedes(t->merged_fxid, horizon))
			{
				uint32 k;

				victim		 = cur;
				victim_prev	 = prev;
				victim_next	 = t->next_page;
				victim_count = t->num_blocks;
				victim_blocks =
						palloc(sizeof(BlockNumber) * Max(victim_count, 1));
				for (k = 0; k < victim_count; k++)
				{
					BlockNumber b = t->blocks[k];

					if (b == 0 || b >= nblocks || b == cur)
					{
						UnlockReleaseBuffer(buf);
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("pg_textsearch: tombstone page %u "
										"lists invalid block %u in index "
										"\"%s\"",
										cur, b,
										RelationGetRelationName(index))));
					}
					victim_blocks[k] = b;
				}
				UnlockReleaseBuffer(buf);
				break;
			}

			prev = cur;
			cur	 = t->next_page;
			UnlockReleaseBuffer(buf);
		}

		if (victim == InvalidBlockNumber)
		{
			if (own_lock)
				tp_release_index_lock(state);
			break; /* nothing left to drain */
		}

		/* Unlink first (corruption-safe; a crash here only leaks). */
		tombstone_unlink(index, victim_prev, victim, victim_next);

		if (own_lock)
			tp_release_index_lock(state);

		/* Now free the victim's blocks + its own page, unlocked. */
		{
			uint32 k;

			for (k = 0; k < victim_count; k++)
				RecordFreeIndexPage(index, victim_blocks[k]);
			RecordFreeIndexPage(index, victim);
			freed += victim_count + 1;
		}
		if (victim_blocks)
			pfree(victim_blocks);
	}

	return freed;
}

uint64
tp_pending_free_block_count(Relation index)
{
	uint64		total = 0;
	BlockNumber cur	  = tp_tombstone_read_head(index);

	while (cur != InvalidBlockNumber)
	{
		Buffer			buf;
		Page			page;
		TpTombstonePage t;
		BlockNumber		next;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(index, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!tp_tombstone_page_is_valid(page))
		{
			UnlockReleaseBuffer(buf);
			break;
		}
		t	  = tp_tombstone_page(page);
		total += t->num_blocks;
		next  = t->next_page;
		UnlockReleaseBuffer(buf);
		cur = next;
	}

	return total;
}
