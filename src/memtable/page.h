/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * page.h - On-disk memtable page format.
 *
 * Phase 1 of the memtable redesign (see issue #374). This module
 * defines the layout of a memtable log page and exposes a minimal
 * set of helpers that operate on a single in-memory page buffer.
 * Concurrency, WAL emission, and chain management are introduced
 * in later phases; everything here is mechanical bytes-on-a-page
 * code.
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
#define TP_MEMTABLE_PAGE_FLAG_DEAD            \
	0x0001 /* page has been spilled or merged \
			* out; dead_fxid records when */

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
	ItemPointerData ctid; /* heap tuple id of the indexed doc */
	uint16			_pad; /* explicit padding so doc_length is 4-aligned */
	int32			doc_length; /* token count of the indexed doc */
	uint32			vector_len; /* byte length of vector_bytes */
	char			vector_bytes[FLEXIBLE_ARRAY_MEMBER];
} TpMemtableRecord;

#define TP_MEMTABLE_RECORD_BASE_SIZE (offsetof(TpMemtableRecord, vector_bytes))

#define TP_MEMTABLE_PAGE_HEADER_OFFSET (MAXALIGN(SizeOfPageHeaderData))

#define TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET \
	(TP_MEMTABLE_PAGE_HEADER_OFFSET + MAXALIGN(sizeof(TpMemtablePageHeader)))

/*
 * Largest vector payload that can fit on an otherwise-empty
 * memtable page.  The writer rejects records whose vector_len
 * exceeds this up front (no buffer or WAL work performed).
 *
 * Derived constant so callers can size sanity-check inputs
 * before touching the writer; the writer also re-checks.
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
