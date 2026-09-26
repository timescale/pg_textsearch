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
#include <storage/indexfsm.h>

#include "constants.h"
#include "index/freepage.h"
#include "index/metapage.h"
#include "index/state.h"
#include "segment/compaction.h"
#include "segment/io.h"
#include "segment/tombstone.h"

void
tp_tombstone_page_init(
		Page page, FullTransactionId merged_fxid, BlockNumber next_page)
{
	TpTombstonePage t;

	PageInit(page, BLCKSZ, 0);

	t			   = tp_tombstone_page(page);
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

static void
tombstone_write_page(
		Relation		   index,
		BlockNumber		   block,
		const BlockNumber *blocks,
		uint32			   start,
		uint32			   count,
		FullTransactionId  merged_fxid,
		BlockNumber		   next_page)
{
	volatile Buffer buf				 = InvalidBuffer;
	GenericXLogState *volatile state = NULL;

	PG_TRY();
	{
		Page			page;
		TpTombstonePage t;

		buf = ReadBuffer(index, block);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		state = GenericXLogStart(index);
		page  = GenericXLogRegisterBuffer(
				 (GenericXLogState *)state, buf, GENERIC_XLOG_FULL_IMAGE);

		tp_tombstone_page_init(page, merged_fxid, next_page);
		t			  = tp_tombstone_page(page);
		t->num_blocks = count;
		for (uint32 i = 0; i < count; i++)
			t->blocks[i] = blocks[start + i];

		GenericXLogFinish((GenericXLogState *)state);
		state = NULL;
		UnlockReleaseBuffer(buf);
		buf = InvalidBuffer;
	}
	PG_CATCH();
	{
		if (state != NULL)
			GenericXLogAbort((GenericXLogState *)state);
		if (BufferIsValid(buf))
		{
			if (InterruptHoldoffCount == 0)
				HOLD_INTERRUPTS();
			UnlockReleaseBuffer(buf);
		}
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
tombstone_build_internal(
		Relation				  index,
		const BlockNumber		 *blocks,
		uint32					  num_blocks,
		FullTransactionId		  merged_fxid,
		BlockNumber				  next_page,
		TpDetachedTombstoneBatch *batch)
{
	uint32 remaining = num_blocks;
	uint32 capacity;

	Assert(batch != NULL);
	memset(batch, 0, sizeof(*batch));
	batch->head = InvalidBlockNumber;
	batch->tail = InvalidBlockNumber;

	if (num_blocks == 0)
		return;

	capacity = num_blocks / TP_TOMBSTONE_CAPACITY +
			   (num_blocks % TP_TOMBSTONE_CAPACITY != 0);
	batch->owned_pages = palloc(sizeof(BlockNumber) * capacity);

	/*
	 * Build the batch tail-first so each page's next_page points at
	 * an already-decided successor.  Active construction is always
	 * detached, so the first page terminates at InvalidBlockNumber.
	 *
	 * Per-page chunking honors TP_TOMBSTONE_CAPACITY.  We assign the
	 * LAST chunk of `blocks` to the first page, walking backwards.
	 */
	while (remaining > 0)
	{
		uint32		chunk = Min(remaining, TP_TOMBSTONE_CAPACITY);
		uint32		start = remaining - chunk;
		BlockNumber blk	  = tp_fsm_claim_or_extend_block(index);

		Assert(batch->owned_count < capacity);
		batch->owned_pages[batch->owned_count++] = blk;
		tp_compaction_allocation_injection_point(
				TP_COMPACTION_ALLOCATION_TOMBSTONE);

		tombstone_write_page(
				index, blk, blocks, start, chunk, merged_fxid, next_page);

		if (batch->tail == InvalidBlockNumber)
			batch->tail = blk;
		batch->head = blk;
		next_page	= blk;
		remaining	= start;
	}
}

void
tp_tombstone_build_detached(
		Relation				  index,
		const BlockNumber		 *blocks,
		uint32					  num_blocks,
		FullTransactionId		  merged_fxid,
		TpDetachedTombstoneBatch *batch)
{
	tombstone_build_internal(
			index, blocks, num_blocks, merged_fxid, InvalidBlockNumber, batch);
}

BlockNumber
tp_tombstone_enqueue_extend(
		Relation		  index,
		BlockNumber		 *blocks,
		uint32			  num_blocks,
		FullTransactionId merged_fxid,
		BlockNumber		  old_head)
{
	volatile TpDetachedTombstoneBatch batch;

	if (num_blocks == 0)
		return old_head;

	memset((TpDetachedTombstoneBatch *)&batch, 0, sizeof(batch));
	PG_TRY();
	{
		tombstone_build_internal(
				index,
				blocks,
				num_blocks,
				merged_fxid,
				old_head,
				(TpDetachedTombstoneBatch *)&batch);
	}
	PG_CATCH();
	{
		tp_tombstone_discard_detached(
				index, *(TpDetachedTombstoneBatch *)&batch);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (batch.owned_pages != NULL)
		pfree((BlockNumber *)batch.owned_pages);
	return batch.head;
}

void
tp_tombstone_restamp_batch(
		Relation				 index,
		TpDetachedTombstoneBatch batch,
		FullTransactionId		 merged_fxid)
{
	for (uint32 i = 0; i < batch.owned_count; i++)
	{
		volatile Buffer buf				 = InvalidBuffer;
		GenericXLogState *volatile state = NULL;

		PG_TRY();
		{
			Page			page;
			TpTombstonePage tombstone;

			buf = ReadBuffer(index, batch.owned_pages[i]);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			state = GenericXLogStart(index);
			page  = GenericXLogRegisterBuffer(
					 (GenericXLogState *)state, buf, 0);
			if (!tp_tombstone_page_is_valid(page))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("pg_textsearch: corrupt detached tombstone "
								"page %u in index \"%s\"",
								batch.owned_pages[i],
								RelationGetRelationName(index))));

			tombstone			   = tp_tombstone_page(page);
			tombstone->merged_fxid = merged_fxid;

			GenericXLogFinish((GenericXLogState *)state);
			state = NULL;
			UnlockReleaseBuffer(buf);
			buf = InvalidBuffer;
		}
		PG_CATCH();
		{
			if (state != NULL)
				GenericXLogAbort((GenericXLogState *)state);
			if (BufferIsValid(buf))
			{
				if (InterruptHoldoffCount == 0)
					HOLD_INTERRUPTS();
				UnlockReleaseBuffer(buf);
			}
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
}

Buffer
tp_tombstone_attach_detached(
		GenericXLogState		*state,
		Relation				 index,
		TpDetachedTombstoneBatch batch,
		BlockNumber				 old_head,
		bool					 allow_invalid_stamp)
{
	volatile Buffer buf = InvalidBuffer;

	Assert(state != NULL);

	if (batch.owned_count == 0)
	{
		Assert(batch.head == InvalidBlockNumber);
		Assert(batch.tail == InvalidBlockNumber);
		return InvalidBuffer;
	}

	Assert(batch.head != InvalidBlockNumber);
	Assert(batch.tail != InvalidBlockNumber);

	PG_TRY();
	{
		Page			page;
		TpTombstonePage t;

		buf = ReadBuffer(index, batch.tail);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = GenericXLogRegisterBuffer(state, buf, 0);

		if (!tp_tombstone_page_is_valid(page))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch: corrupt detached tombstone tail "
							"page %u in index \"%s\"",
							batch.tail,
							RelationGetRelationName(index))));

		t = tp_tombstone_page(page);
		if (t->next_page != InvalidBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch: detached tombstone tail page %u "
							"is already attached",
							batch.tail)));
		if (!allow_invalid_stamp && !FullTransactionIdIsValid(t->merged_fxid))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("detached tombstone batch has no publication "
							"reclaim stamp")));

		t->next_page = old_head;
	}
	PG_CATCH();
	{
		if (BufferIsValid(buf))
		{
			if (InterruptHoldoffCount == 0)
				HOLD_INTERRUPTS();
			UnlockReleaseBuffer(buf);
		}
		PG_RE_THROW();
	}
	PG_END_TRY();

	return (Buffer)buf;
}

void
tp_tombstone_discard_detached(Relation index, TpDetachedTombstoneBatch batch)
{
	for (uint32 i = 0; i < batch.owned_count; i++)
		tp_record_free_index_page(index, batch.owned_pages[i]);

	if (batch.owned_count > 0)
		IndexFreeSpaceMapVacuum(index);
	if (batch.owned_pages != NULL)
		pfree(batch.owned_pages);
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
	if (metap->version < TP_METAPAGE_VERSION_V8)
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

	(void)victim;

	state = GenericXLogStart(index);

	if (prev == InvalidBlockNumber)
	{
		TpIndexMetaPage metap;

		buf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = GenericXLogRegisterBuffer(state, buf, 0);
		tp_metapage_upgrade_to_current(index, page);
		metap					 = (TpIndexMetaPage)PageGetContents(page);
		metap->pending_free_head = victim_next;
	}
	else
	{
		TpTombstonePage t;

		buf = ReadBuffer(index, prev);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
		t	 = tp_tombstone_page(page);
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
	uint32 freed = 0;

	/* own_lock=true needs `state` to acquire and release the lock. */
	Assert(!own_lock || state != NULL);

	for (;;)
	{
		BlockNumber		  nblocks;
		BlockNumber		  prev = InvalidBlockNumber;
		BlockNumber		  cur;
		BlockNumber		  victim			 = InvalidBlockNumber;
		BlockNumber		  victim_prev		 = InvalidBlockNumber;
		BlockNumber		  victim_next		 = InvalidBlockNumber;
		BlockNumber		 *unstamped_pages	 = NULL;
		uint32			  unstamped_count	 = 0;
		uint32			  unstamped_capacity = 0;
		BlockNumber		 *victim_blocks		 = NULL;
		uint32			  victim_count		 = 0;
		FullTransactionId victim_fxid		 = InvalidFullTransactionId;
		bool			  corrupt			 = false;
		BlockNumber		  corrupt_at		 = InvalidBlockNumber;
		BlockNumber		  corrupt_prev		 = InvalidBlockNumber;

		CHECK_FOR_INTERRUPTS();

		if (own_lock)
			tp_acquire_index_lock(state, LW_EXCLUSIVE);

		/*
		 * Re-read under the lock each iteration: the lock is dropped
		 * between iterations and the relation can shrink.  This only
		 * keeps the b >= nblocks check below honest; it is not what
		 * makes the frees safe (see below).
		 */
		nblocks = RelationGetNumberOfBlocks(index);

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
				corrupt		 = true;
				corrupt_at	 = cur;
				corrupt_prev = prev;
				break;
			}

			t = tp_tombstone_page(page);
			if (!FullTransactionIdIsValid(t->merged_fxid))
			{
				/*
				 * Collect the whole contiguous unstamped run;
				 * restamping one page per chain walk is quadratic
				 * in the number of tombstones.
				 */
				BlockNumber next = t->next_page;

				if (unstamped_count == unstamped_capacity)
				{
					unstamped_capacity = unstamped_capacity == 0
											   ? 8
											   : unstamped_capacity * 2;
					unstamped_pages =
							unstamped_pages == NULL
									? palloc(sizeof(BlockNumber) *
											 unstamped_capacity)
									: repalloc(
											  unstamped_pages,
											  sizeof(BlockNumber) *
													  unstamped_capacity);
				}
				unstamped_pages[unstamped_count++] = cur;
				UnlockReleaseBuffer(buf);
				cur = next;
				continue;
			}
			if (unstamped_count > 0)
			{
				/* Run ended; restamp it before considering a victim. */
				UnlockReleaseBuffer(buf);
				break;
			}
			if (FullTransactionIdPrecedes(t->merged_fxid, horizon))
			{
				uint32 k;

				victim		  = cur;
				victim_prev	  = prev;
				victim_next	  = t->next_page;
				victim_count  = t->num_blocks;
				victim_fxid	  = t->merged_fxid;
				victim_blocks = palloc(
						sizeof(BlockNumber) * Max(victim_count, 1));
				for (k = 0; k < victim_count; k++)
				{
					BlockNumber b = t->blocks[k];

					if (b == 0 || b >= nblocks || b == cur)
					{
						corrupt		 = true;
						corrupt_at	 = cur;
						corrupt_prev = prev;
						break;
					}
					victim_blocks[k] = b;
				}
				UnlockReleaseBuffer(buf);
				if (corrupt)
				{
					pfree(victim_blocks);
					victim_blocks = NULL;
					victim		  = InvalidBlockNumber;
				}
				break;
			}

			prev = cur;
			cur	 = t->next_page;
			UnlockReleaseBuffer(buf);
		}

		if (unstamped_count > 0)
		{
			TpDetachedTombstoneBatch batch;

			memset(&batch, 0, sizeof(batch));
			batch.head		  = unstamped_pages[0];
			batch.tail		  = unstamped_pages[unstamped_count - 1];
			batch.owned_pages = unstamped_pages;
			batch.owned_count = unstamped_count;
			tp_tombstone_restamp_batch(
					index, batch, ReadNextFullTransactionId());
			pfree(unstamped_pages);
			if (own_lock)
				tp_release_index_lock(state);
			continue;
		}

		/*
		 * Self-heal an already-corrupt chain instead of wedging every
		 * writer (issue #427).  An older page-reuse bug (or a crash
		 * with a stale FSM) can leave a chain node that no longer
		 * validates, or a valid node that lists an impossible block.
		 * We cannot trust such a node's next_page, so drop it and the
		 * unverifiable remainder by pointing its predecessor — a
		 * still-valid tombstone page, or the metapage head when the
		 * corruption is at the head — at InvalidBlockNumber.  The
		 * still-drainable prefix ahead of the corruption is preserved;
		 * the dropped tail leaks pages that a REINDEX reclaims.  We
		 * never free the corrupt node's listed blocks, so if the block
		 * was double-owned by a live structure, healing cannot corrupt
		 * that structure.
		 */
		if (corrupt)
		{
			tombstone_unlink(
					index, corrupt_prev, corrupt_at, InvalidBlockNumber);

			if (own_lock)
				tp_release_index_lock(state);

			ereport(WARNING,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch: recovered from corrupt tombstone "
							"page %u in index \"%s\"",
							corrupt_at,
							RelationGetRelationName(index)),
					 errdetail(
							 "Dropped the deferred-free chain from that "
							 "page onward; leaked index pages are "
							 "reclaimed by REINDEX.")));
			break;
		}

		if (victim == InvalidBlockNumber)
		{
			if (own_lock)
				tp_release_index_lock(state);
			break; /* nothing left to drain */
		}

		tp_log_page_reuse_conflict(
				index,
				victim_count > 0 ? victim_blocks[0] : victim,
				victim_fxid);

		/* Unlink first (corruption-safe; a crash here only leaks). */
		tombstone_unlink(index, victim_prev, victim, victim_next);

		/*
		 * Free under the same lock as the unlink so no other index
		 * mutation observes an intermediate state.  Freeing before the
		 * unlink is unsafe: a still-chained tombstone's blocks could be
		 * claimed from the FSM, then freed again by a later drain.
		 *
		 * No CHECK_FOR_INTERRUPTS here — the tombstone is already
		 * unlinked, so erroring part-way strands the rest.  The loop
		 * is bounded by TP_TOMBSTONE_CAPACITY.
		 */
		Assert(!own_lock ||
			   LWLockHeldByMeInMode(&state->shared->lock, LW_EXCLUSIVE));
		{
			uint32 k;

			for (k = 0; k < victim_count; k++)
				tp_record_free_index_page(index, victim_blocks[k]);
			tp_record_free_index_page(index, victim);
			freed += victim_count + 1;
		}

		if (own_lock)
			tp_release_index_lock(state);

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

	/*
	 * Caller must hold the per-index LWLock in shared mode so a concurrent
	 * drain/enqueue can't recycle a tombstone page mid-walk (see
	 * tp_pending_free_pages in dump.c).  Under that lock an invalid page
	 * means real corruption, so ERROR rather than return a short count.
	 */
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
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch: corrupt tombstone page %u "
							"in index \"%s\"",
							cur,
							RelationGetRelationName(index))));
		}
		t = tp_tombstone_page(page);
		total += t->num_blocks;
		next = t->next_page;
		UnlockReleaseBuffer(buf);
		cur = next;
	}

	return total;
}
