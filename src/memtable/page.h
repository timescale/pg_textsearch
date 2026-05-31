/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * page.h - On-disk memtable page format.
 *
 * Memtable v2 (see issue #374). This module defines the layout
 * of a memtable log page and exposes a minimal set of helpers
 * that operate on a single in-memory page buffer.  Concurrency,
 * WAL emission, and chain management live one layer up in log.c;
 * everything here is mechanical bytes-on-a-page code.
 *
 * Layout summary (offsets shown for BLCKSZ=8192, MAXALIGN=8):
 *
 *   offset 0..23           PageHeaderData (managed by PageInit)
 *   offset 24..47          TpMemtablePageHeader (this file)
 *   offset 48..free_offset Packed TpMemtableRecord stream
 *   offset free_offset..   Free space
 *
 * Each record is laid out as:
 *
 *   ItemPointerData ctid        (6 bytes, 2-byte aligned)
 *   <2 bytes padding>
 *   int32           doc_length  (4 bytes)
 *   uint32          vector_len  (4 bytes)
 *   char            vector_bytes[vector_len]
 *   <padding to MAXALIGN>
 *
 * The vector_bytes payload is an opaque blob from this layer's
 * point of view; the write path will populate it with the same
 * bytes as the v2 TpVector wire format so that the read path can
 * walk it without an intermediate palloc.
 */
#pragma once

#include <postgres.h>

#include <access/transam.h>
#include <storage/block.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>

#include "constants.h"

/* Flags occupying the `flags` field of TpMemtablePageHeader. */
#define TP_MEMTABLE_PAGE_FLAG_DEAD              \
	0x0001 /* unlinked from metapage; dead_fxid \
			* is spill horizon for FSM recycle */
/*
 * Page contains continuation bytes of a previous record's
 * oversized payload.  n_records is 0 and the on-page bytes
 * between [first_record_offset, free_offset) form an opaque
 * chunk of payload.  See tp_memtable_continuation_page_init().
 */
#define TP_MEMTABLE_PAGE_FLAG_CONTINUATION 0x0002

/* Flags occupying the `flags` field of TpMemtableRecord. */
/*
 * Record's vector payload spans this page + one or more
 * following continuation pages.  vector_len stores the FULL
 * payload length; the inline byte count on this page is
 * implicit (page_free_offset - record_offset - base_size).
 * The record is always the sole record on its head page; the
 * head page's next_block points to the first continuation.
 */
#define TP_MEMTABLE_RECORD_FLAG_FRAGMENT 0x0001

/*
 * Tapir memtable page header.
 *
 * Placed at PageGetContents(page).  Total size is 24 bytes and is
 * naturally aligned.  Future flag bits or version bumps can extend
 * the unused space without breaking the read/write helpers.
 */
typedef struct TpMemtablePageHeader
{
	uint32 magic;				 /* TP_MEMTABLE_PAGE_MAGIC */
	uint16 version;				 /* TP_MEMTABLE_PAGE_VERSION */
	uint16 flags;				 /* see TP_MEMTABLE_PAGE_FLAG_* */
	uint16 n_records;			 /* number of records on the page */
	uint16 free_offset;			 /* byte offset (from start of page) of the
								  * next record write position; always
								  * MAXALIGN'd */
	BlockNumber next_block;		 /* next page in the chain, or
								  * InvalidBlockNumber */
	FullTransactionId dead_fxid; /* valid only when flags & DEAD: the xid
								  * horizon after which the page may be
								  * FSM-recycled */
} TpMemtablePageHeader;

/*
 * Tapir memtable record.
 *
 * One record per indexed document.  Variable length; the trailing
 * vector_bytes payload is the v2 TpVector wire format (write path
 * will use TPVECTOR_VARSIZE(...) bytes copied verbatim).
 */
typedef struct TpMemtableRecord
{
	ItemPointerData ctid;		/* heap tuple id of the indexed doc */
	uint16			flags;		/* see TP_MEMTABLE_RECORD_FLAG_* */
	int32			doc_length; /* token count of the indexed doc */
	uint32			vector_len; /* byte length of full vector payload
								 * (may exceed the inline byte count
								 * when FRAGMENT flag is set) */
	char vector_bytes[FLEXIBLE_ARRAY_MEMBER];
} TpMemtableRecord;

#define TP_MEMTABLE_RECORD_BASE_SIZE (offsetof(TpMemtableRecord, vector_bytes))

#define TP_MEMTABLE_PAGE_HEADER_OFFSET (MAXALIGN(SizeOfPageHeaderData))

#define TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET \
	(TP_MEMTABLE_PAGE_HEADER_OFFSET + MAXALIGN(sizeof(TpMemtablePageHeader)))

/*
 * Largest vector payload that can fit on an otherwise-empty
 * memtable page as a single (non-fragmented) record.  Records
 * whose vector_len exceeds this are written via the FRAGMENT
 * path: a head record on its own page followed by one or more
 * continuation pages.
 *
 * Derived constant so callers can sanity-check inputs before
 * touching the writer; the writer also re-checks.
 */
#define TP_MEMTABLE_PAGE_MAX_VECTOR_LEN                         \
	((uint32)((BLCKSZ - TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET) - \
			  TP_MEMTABLE_RECORD_BASE_SIZE))

/* Return the on-page header.  Caller must hold the buffer pinned. */
extern TpMemtablePageHeader *tp_memtable_page_header(Page page);

/*
 * Initialize an empty memtable page in `page`.  The page must
 * already have been zeroed and laid out by PageInit(...) with
 * specialSize==0.  After this call the page reports n_records=0,
 * next_block=InvalidBlockNumber, flags=0, and is recognized as
 * valid by tp_memtable_page_is_valid().
 */
extern void tp_memtable_page_init(Page page);

/*
 * Returns true iff `page` carries a recognizable memtable page
 * header (magic + version match).  Used to detect uninitialized
 * or recycled pages.
 */
extern bool tp_memtable_page_is_valid(Page page);

/*
 * Total on-page space (including padding) consumed by a record
 * whose vector payload is `vector_len` bytes.
 */
extern uint32 tp_memtable_record_size(uint32 vector_len);

/*
 * Returns true iff appending a record with vector payload
 * `vector_len` would fit on the page.
 */
extern bool tp_memtable_page_can_fit(Page page, uint32 vector_len);

/*
 * Append a record to the page.  Caller must have established
 * via tp_memtable_page_can_fit() that the record fits; this
 * function will ereport(ERROR) if it does not.  Returns a
 * pointer into the page at the newly written record.
 */
extern TpMemtableRecord *tp_memtable_page_append(
		Page		page,
		ItemPointer ctid,
		int32		doc_length,
		const char *vector_bytes,
		uint32		vector_len);

/*
 * Iteration: returns a pointer to the first record on the page,
 * or NULL if the page is empty.
 */
extern TpMemtableRecord *tp_memtable_page_first(Page page);

/*
 * Returns the next record after `cur`, or NULL if `cur` is the
 * last record on the page.
 */
extern TpMemtableRecord *
tp_memtable_page_next(Page page, TpMemtableRecord *cur);

/*
 * Chain-pointer accessors.  Storing InvalidBlockNumber means "tail
 * of chain".  These touch only the header.
 */
extern BlockNumber tp_memtable_page_get_next(Page page);
extern void		   tp_memtable_page_set_next(Page page, BlockNumber blk);

/* Number of records currently on the page. */
extern uint16 tp_memtable_page_n_records(Page page);

/* ---------- multi-page (fragment) record support ---------- */

/*
 * Inline payload bytes that a FRAGMENT head record can store on
 * an otherwise-empty page.  The remainder of the payload lives
 * on continuation pages (chained via the head page's next_block,
 * each continuation's own next_block).
 *
 * The fragment writer always sizes the inline prefix to this
 * maximum so the head page contains exactly one (FRAGMENT)
 * record and has no remaining free space — the page's
 * free_offset advances to BLCKSZ.
 */
extern uint32 tp_memtable_fragment_inline_capacity_max(void);

/*
 * Append a FRAGMENT head record onto an otherwise-empty page.
 * `total_vector_len` is the full payload length (must exceed
 * TP_MEMTABLE_PAGE_MAX_VECTOR_LEN — caller ensures this).
 * `prefix_len` bytes from `prefix` are copied inline; the
 * remainder lives on continuation pages the caller will
 * populate separately.
 *
 * `prefix_len` must equal tp_memtable_fragment_inline_capacity_max()
 * (i.e. fill the page) — the caller is responsible for slicing
 * the input payload accordingly.
 *
 * Returns a pointer into the page at the newly written record.
 */
extern TpMemtableRecord *tp_memtable_page_append_fragment_head(
		Page		page,
		ItemPointer ctid,
		int32		doc_length,
		const char *prefix,
		uint32		prefix_len,
		uint32		total_vector_len);

/*
 * Maximum payload chunk size, in bytes, that fits on a single
 * continuation page.  Continuation pages carry no records;
 * they are pure payload immediately following the page header.
 */
extern uint32 tp_memtable_continuation_max_chunk(void);

/*
 * Initialize `page` as a continuation page carrying `chunk_len`
 * bytes of payload (`chunk` may be NULL only when chunk_len==0)
 * and linking forward to `next_block`.  The page must already
 * have been zeroed and laid out by PageInit().  Caller is
 * responsible for ensuring chunk_len <= continuation_max_chunk().
 */
extern void tp_memtable_continuation_page_init(
		Page		page,
		const char *chunk,
		uint32		chunk_len,
		BlockNumber next_block);

/*
 * Returns true iff `page` carries a valid memtable page header
 * with the CONTINUATION flag set.
 */
extern bool tp_memtable_page_is_continuation(Page page);

/*
 * Return a pointer to the chunk bytes stored on a continuation
 * page; writes the chunk length to *out_len.  Caller holds the
 * buffer pinned (and at least SHARED-locked) for the bytes'
 * lifetime.
 */
extern const char *
tp_memtable_continuation_page_chunk(Page page, uint32 *out_len);

/*
 * DEAD flag helpers (page buffer / GenericXLog working copy).
 * Idempotent: first horizon wins when marking dead.
 */
extern void tp_memtable_page_mark_dead(Page page, FullTransactionId horizon);
extern bool tp_memtable_page_is_dead(Page page);
