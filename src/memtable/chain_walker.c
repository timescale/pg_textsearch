/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * chain_walker.c - resumable iterator over the on-disk memtable
 *                  page chain.  See chain_walker.h for the API
 *                  contract, concurrency assumptions, and cursor
 *                  semantics.
 *
 * Implementation notes.
 *
 * - The walker holds at most one chain page buffer at a time
 *   (SHARED).  Multiple records on the same page are read under
 *   one buffer-lock acquisition; the buffer is released at the
 *   page boundary, before opening the next page.
 *
 * - FRAGMENT reassembly: when the walker hits a FRAGMENT head
 *   record, it walks the continuation chain in-line (each
 *   continuation page is opened SHARED, its chunk memcpy'd into
 *   the caller's mcxt, then released) and returns the reassembled
 *   payload as the record's vector_bytes.  After yielding, the
 *   walker's pending next-page-to-open is the post-fragment
 *   regular page (Tnew in the writer's terminology — always a
 *   regular page, never InvalidBlockNumber, established by
 *   memtable_append_fragment in src/memtable/log.c).
 *
 * - Validation: every page validates magic, free_offset range,
 *   self-loop, and (for fragment heads) n_records == 1.  Every
 *   inline record validates that its tail fits within the page's
 *   free_offset.  These match the defenses in the predecessor
 *   walk_chain() in chain_source.c, where they have been
 *   stable in production.
 */
#include <postgres.h>

#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "memtable/chain_walker.h"
#include "memtable/page.h"
#include "types/vector.h"

struct TpChainWalker
{
	Relation	  rel;
	MemoryContext mcxt;

	/* Currently-held page (Invalid means no page held). */
	Buffer cur_buf;

	/*
	 * State of the currently-held page.  Only meaningful while
	 * cur_buf != InvalidBuffer; cached up front so we don't have
	 * to re-read the page header on every record.
	 */
	BlockNumber cur_blkno;
	uint16		cur_off;		 /* next record offset to read */
	uint16		cur_free_offset; /* page free_offset (stop here) */
	BlockNumber cur_next_block;	 /* page's hdr->next_block */

	/*
	 * Next page to open when cur_buf is released (or on the
	 * first call).  InvalidBlockNumber means "walker is done".
	 */
	BlockNumber pending_blkno;

	/*
	 * Pending start offset for the next page we open.  Honored
	 * only on the first page we open (i.e. when the walker was
	 * given a non-zero start_off at _open() time).  After that,
	 * every fresh page starts at TP_MEMTABLE_PAGE_FIRST_RECORD_
	 * OFFSET.
	 */
	uint16 pending_off;

	uint32 pages_visited;
};

static void
validate_page_layout(Page page, BlockNumber blkno)
{
	TpMemtablePageHeader *hdr = tp_memtable_page_header(page);

	if (hdr->free_offset < TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET ||
		hdr->free_offset > BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch memtable page block %u has "
						"out-of-range free_offset %u",
						blkno,
						hdr->free_offset)));

	if (hdr->next_block == blkno)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch memtable page block %u links to "
						"itself",
						blkno)));
}

/*
 * Open `blkno` as the walker's current page.  Acquires
 * BUFFER_LOCK_SHARE and caches header fields into walker state.
 * `start_off` is the byte offset to begin reading at on this
 * page (clamped up to FIRST_RECORD_OFFSET).
 */
static void
open_page(TpChainWalker *w, BlockNumber blkno, uint16 start_off)
{
	Buffer				  buf;
	Page				  page;
	TpMemtablePageHeader *hdr;

	Assert(w->cur_buf == InvalidBuffer);
	Assert(BlockNumberIsValid(blkno));

	CHECK_FOR_INTERRUPTS();

	buf = ReadBuffer(w->rel, blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);

	if (!tp_memtable_page_is_valid(page))
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch memtable page at block %u in "
						"index \"%s\" has invalid magic",
						blkno,
						RelationGetRelationName(w->rel))));
	}

	/*
	 * The outer chain walk only ever opens regular pages —
	 * cursors are never planted on continuation pages by any
	 * code path (the writer's fragment protocol leaves a fresh
	 * regular Tnew as the post-fragment tail, and that's what
	 * the walker stores into pending_blkno after a fragment).
	 * If we land on a continuation page here it indicates either
	 * a corrupt next_block link or a caller-supplied cursor
	 * pointing into a fragment's interior; both are errors.
	 */
	if (tp_memtable_page_is_continuation(page))
	{
		UnlockReleaseBuffer(buf);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch memtable outer walk reached "
						"continuation page at block %u in index "
						"\"%s\"",
						blkno,
						RelationGetRelationName(w->rel))));
	}

	validate_page_layout(page, blkno);

	hdr				   = tp_memtable_page_header(page);
	w->cur_buf		   = buf;
	w->cur_blkno	   = blkno;
	w->cur_free_offset = hdr->free_offset;
	w->cur_next_block  = hdr->next_block;
	w->cur_off		   = Max(start_off, TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET);
	w->pages_visited++;
}

static void
release_cur_page(TpChainWalker *w)
{
	if (w->cur_buf != InvalidBuffer)
	{
		UnlockReleaseBuffer(w->cur_buf);
		w->cur_buf = InvalidBuffer;
	}
}

/*
 * Reassemble a FRAGMENT record's payload from continuation
 * pages into a palloc'd buffer in walker mcxt.  Returns the
 * BlockNumber of the post-fragment page (Cn->next_block), which
 * the writer guarantees to be a freshly-initialized regular
 * page (Tnew, never Invalid).
 *
 * On entry, w->cur_buf holds the fragment-head page SHARED and
 * `head_rec` points at the fragment head record on that page.
 * On exit, the fragment-head page is still held SHARED; the
 * caller releases it.
 */
static BlockNumber
reassemble_fragment(
		TpChainWalker *w, TpMemtableRecord *head_rec, char **out_buf)
{
	Page				  head_page = BufferGetPage(w->cur_buf);
	TpMemtablePageHeader *hdr		= tp_memtable_page_header(head_page);
	uint32				  full_len	= head_rec->vector_len;
	char				 *vec_start = (char *)head_rec->vector_bytes;
	char				 *page_end	= (char *)head_page + hdr->free_offset;
	uint32				  inline_len;
	char				 *full;
	uint32				  copied;
	BlockNumber			  cont_blkno;
	MemoryContext		  old;

	if (page_end <= vec_start)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch fragment head record has no inline "
						"payload")));

	inline_len = (uint32)(page_end - vec_start);

	if (inline_len >= full_len)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_textsearch fragment record has inline_len=%u "
						">= full_len=%u (should have been a regular record)",
						inline_len,
						full_len)));

	old	 = MemoryContextSwitchTo(w->mcxt);
	full = palloc(full_len);
	MemoryContextSwitchTo(old);

	memcpy(full, head_rec->vector_bytes, inline_len);
	copied	   = inline_len;
	cont_blkno = hdr->next_block;

	while (copied < full_len)
	{
		Buffer		cbuf;
		Page		cpage;
		const char *chunk;
		uint32		chunk_len;
		BlockNumber next_blk;

		CHECK_FOR_INTERRUPTS();

		if (cont_blkno == InvalidBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch fragment continuation chain "
							"truncated: copied=%u, expected=%u",
							copied,
							full_len)));

		cbuf = ReadBuffer(w->rel, cont_blkno);
		LockBuffer(cbuf, BUFFER_LOCK_SHARE);
		cpage = BufferGetPage(cbuf);

		if (!tp_memtable_page_is_continuation(cpage))
		{
			UnlockReleaseBuffer(cbuf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch fragment continuation at block "
							"%u is not a continuation page",
							cont_blkno)));
		}

		chunk = tp_memtable_continuation_page_chunk(cpage, &chunk_len);
		if (chunk_len == 0 || copied + chunk_len > full_len)
		{
			UnlockReleaseBuffer(cbuf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch fragment continuation at block "
							"%u has invalid chunk_len=%u (copied=%u, "
							"full_len=%u)",
							cont_blkno,
							chunk_len,
							copied,
							full_len)));
		}

		memcpy(full + copied, chunk, chunk_len);
		copied += chunk_len;
		next_blk = tp_memtable_page_get_next(cpage);
		UnlockReleaseBuffer(cbuf);
		cont_blkno = next_blk;
		w->pages_visited++;
	}

	*out_buf = full;
	/*
	 * cont_blkno is now the LAST continuation's next_block —
	 * the writer (memtable_append_fragment) initializes this as
	 * Tnew, a fresh regular page that becomes the new chain
	 * tail.  Assert in debug builds that it is in fact valid;
	 * production data corruption surfaces at the next open_page
	 * via validate_page_layout / is_valid checks.
	 */
	Assert(BlockNumberIsValid(cont_blkno));
	return cont_blkno;
}

TpChainWalker *
tp_chain_walker_open(
		Relation	  rel,
		BlockNumber	  start_blkno,
		uint16		  start_off,
		MemoryContext mcxt)
{
	TpChainWalker *w;

	Assert(rel != NULL);
	Assert(mcxt != NULL);

	w = (TpChainWalker *)palloc0(sizeof(TpChainWalker));

	w->rel			 = rel;
	w->mcxt			 = mcxt;
	w->cur_buf		 = InvalidBuffer;
	w->pending_blkno = start_blkno;
	w->pending_off	 = start_off;

	return w;
}

void
tp_chain_walker_close(TpChainWalker *w)
{
	if (w == NULL)
		return;
	release_cur_page(w);
	pfree(w);
}

uint32
tp_chain_walker_pages_visited(const TpChainWalker *w)
{
	return w == NULL ? 0 : w->pages_visited;
}

bool
tp_chain_walker_next(TpChainWalker *w, TpChainWalkerRecord *out)
{
	Assert(w != NULL);
	Assert(out != NULL);

	for (;;)
	{
		Page			  page;
		TpMemtableRecord *rec;

		/*
		 * If no page is currently held, try to open the pending
		 * one; if there's no pending page either, walker is
		 * exhausted.
		 */
		if (w->cur_buf == InvalidBuffer)
		{
			BlockNumber to_open = w->pending_blkno;
			uint16		off;

			if (!BlockNumberIsValid(to_open))
				return false;

			w->pending_blkno = InvalidBlockNumber;
			off				 = w->pending_off;
			w->pending_off	 = 0;
			open_page(w, to_open, off);
		}

		/* No more records on this page?  Release and follow next_block. */
		if (w->cur_off >= w->cur_free_offset)
		{
			w->pending_blkno = w->cur_next_block;
			w->pending_off	 = 0;
			release_cur_page(w);
			continue;
		}

		page = BufferGetPage(w->cur_buf);
		rec	 = (TpMemtableRecord *)((char *)page + w->cur_off);

		if ((rec->flags & TP_MEMTABLE_RECORD_FLAG_FRAGMENT) != 0)
		{
			BlockNumber			  post_cont;
			char				 *full;
			TpMemtablePageHeader *hdr = tp_memtable_page_header(page);

			if (hdr->n_records != 1)
			{
				release_cur_page(w);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("pg_textsearch fragment head page %u "
								"has n_records=%u (must be 1)",
								w->cur_blkno,
								hdr->n_records)));
			}

			post_cont = reassemble_fragment(w, rec, &full);

			/*
			 * A multi-page fragment payload has just been
			 * stitched together across multiple buffer-locked
			 * page reads.  Run a full bounds-check
			 * unconditionally — the defensive value is much
			 * higher than for an inline record where we can
			 * lean on the page CRC and the same-backend write
			 * rationale; the cost is amortised over a payload
			 * that is by definition larger than a single page.
			 * Without this check, a structurally-malformed
			 * `full` buffer would silently propagate into the
			 * caller's downstream decoder.
			 */
			tpvector_validate_v2_buffer(full, rec->vector_len);

			out->ctid		  = rec->ctid;
			out->doc_length	  = rec->doc_length;
			out->vector_bytes = full;
			out->vector_len	  = rec->vector_len;
			out->is_fragment  = true;
			out->next_blkno	  = post_cont;
			out->next_off	  = TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET;

			/*
			 * Release the fragment-head page and stage the
			 * post-fragment regular page for the next call.
			 */
			release_cur_page(w);
			w->pending_blkno = post_cont;
			w->pending_off	 = 0;
			return true;
		}

		/* Inline record: validate that its tail fits in the page. */
		{
			char *rec_end = (char *)rec +
							tp_memtable_record_size(rec->vector_len);
			char *page_end_bytes = (char *)page + w->cur_free_offset;

			if (rec_end > page_end_bytes)
			{
				release_cur_page(w);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("pg_textsearch memtable record at block "
								"%u, offset %u extends past free_offset",
								w->cur_blkno,
								w->cur_off)));
			}
		}

		out->ctid		  = rec->ctid;
		out->doc_length	  = rec->doc_length;
		out->vector_bytes = rec->vector_bytes;
		out->vector_len	  = rec->vector_len;
		out->is_fragment  = false;

		/* Advance our internal cur_off. */
		w->cur_off += tp_memtable_record_size(rec->vector_len);

		/*
		 * Report the cursor: if there are more records on this
		 * page (or the page is the tail and may grow), anchor at
		 * the same page.  If this page is sealed (next_block is
		 * valid) and we just consumed its last record, advance
		 * to the start of the next page so resume doesn't pay
		 * an unnecessary ReadBuffer on the sealed page.
		 */
		if (w->cur_off >= w->cur_free_offset &&
			BlockNumberIsValid(w->cur_next_block))
		{
			out->next_blkno = w->cur_next_block;
			out->next_off	= TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET;
		}
		else
		{
			out->next_blkno = w->cur_blkno;
			out->next_off	= w->cur_off;
		}

		return true;
	}
}
