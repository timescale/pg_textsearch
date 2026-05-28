/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * chain_walker.h - resumable iterator over the on-disk memtable
 *                  page chain.
 *
 * The walker reads document records from the chain page-by-page
 * (acquiring buffer SHARED, parsing records, transparently
 * reassembling FRAGMENT payloads across continuation pages, then
 * releasing the buffer at page boundaries).  It is the shared
 * primitive used by both:
 *
 *   - chain_source.c: an eager-walk-the-whole-chain
 *     TpDataSource (today's read/spill code path), and
 *   - cache.c (memtable in-memory cache, #374 follow-up):
 *     incrementally catches the cache up to the current chain
 *     tail by resuming from a stored cursor position.
 *
 * Concurrency: matches chain_source's contract.  Caller MUST
 * hold the per-index LWLock SHARED (or stronger) so spill
 * cannot race the walk.  The walker takes chain page buffer
 * SHARED one page at a time; multiple records on the same
 * page are read under one buffer-lock acquisition and
 * released at the page boundary.
 *
 * Cursor semantics.  Each call to tp_chain_walker_next() that
 * returns true populates out->next_blkno / out->next_off with
 * the LOGICAL position of the next record to read.  Saving
 * (next_blkno, next_off) and reopening a fresh walker at that
 * position picks up exactly where the previous walker left off
 * — even across concurrent appends that extend the same tail
 * page or chain a new page.  See docs/memtable_cache.md
 * "The apply cursor" for the full rationale, including why
 * fragment-page allocation order (which can produce
 * BlockNumber(Tnew) < BlockNumber(Thead)) does not desync the
 * cursor: the walker follows logical links, never block-number
 * comparisons.
 *
 * Lifetime of out->vector_bytes: for inline records, the
 * pointer is into the buffer page the walker currently holds
 * SHARED; valid until the next call to tp_chain_walker_next()
 * or tp_chain_walker_close().  For FRAGMENT records, the
 * reassembled payload is palloc'd in the MemoryContext the
 * caller passed at _open() time, and survives walker advance
 * (caller is responsible for any pfree).
 */
#pragma once

#include <postgres.h>

#include <storage/block.h>
#include <storage/itemptr.h>
#include <utils/memutils.h>
#include <utils/rel.h>

typedef struct TpChainWalker TpChainWalker;

/*
 * One record yielded by the walker.
 *
 * (ctid, doc_length, vector_bytes, vector_len) are decoded from
 * the on-disk record; (next_blkno, next_off) is the cursor
 * position the next call to tp_chain_walker_next() (in this or a
 * subsequent walker) should resume from.
 *
 * When the LAST record on cur_page is read and cur_page has a
 * non-Invalid next_block (i.e. the page is sealed), next_(blkno,
 * off) advances to (next_block, FIRST_RECORD_OFFSET) — there's
 * no reason to anchor the cursor on a sealed page, and advancing
 * saves an unnecessary ReadBuffer next time.  When cur_page is
 * the chain tail (next_block == Invalid), the cursor anchors at
 * (cur_blkno, free_offset) so future writes that extend the
 * tail page in place are picked up on resume.
 */
typedef struct TpChainWalkerRecord
{
	ItemPointerData ctid;
	int32			doc_length;
	const char	   *vector_bytes;
	uint32			vector_len;

	/*
	 * True iff this record's payload was reassembled from one
	 * or more continuation pages (FRAGMENT).  The walker has
	 * already run tpvector_validate_v2_buffer() on the
	 * reassembled buffer in this case, so callers do not need
	 * to re-validate on the fragment path.  For inline records
	 * (is_fragment == false), the caller may want to run
	 * assert-mode validation via USE_ASSERT_CHECKING.
	 */
	bool is_fragment;

	BlockNumber next_blkno;
	uint16		next_off;
} TpChainWalkerRecord;

/*
 * Open a walker positioned at (start_blkno, start_off).
 *
 * start_blkno == InvalidBlockNumber yields an immediately-empty
 * walker (the first _next() returns false).  start_off is
 * clamped up to TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET — passing 0
 * is the natural "start at beginning of page" sentinel used by
 * cold-build callers seeding the cursor from meta.head_blkno.
 *
 * `mcxt` is the MemoryContext used for fragment-payload
 * reassembly buffers.  The walker does not allocate from `mcxt`
 * for inline records (those are zero-copy into the buffer
 * page).
 *
 * Returns a heap-allocated walker; close with
 * tp_chain_walker_close() to release any held buffer SHARED lock.
 */
extern TpChainWalker *tp_chain_walker_open(
		Relation	  rel,
		BlockNumber	  start_blkno,
		uint16		  start_off,
		MemoryContext mcxt);

/*
 * Advance the walker by one record.  On true, *out is populated
 * with the record and the next cursor position.  On false, the
 * walker has reached the end of the chain at the time of the
 * call; it is safe to call again later (the chain may have
 * grown).  However, the simpler pattern is to close and reopen
 * the walker at the last reported (next_blkno, next_off).
 */
extern bool tp_chain_walker_next(TpChainWalker *w, TpChainWalkerRecord *out);

/*
 * Total chain pages visited so far (regular + continuation).
 * Used by chain_source.c to seed shared->chain_page_count.
 * Continuation pages are counted in the page where they are
 * read during fragment reassembly; the head page is counted
 * exactly once.
 */
extern uint32 tp_chain_walker_pages_visited(const TpChainWalker *w);

extern void tp_chain_walker_close(TpChainWalker *w);
