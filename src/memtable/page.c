/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * page.c - On-disk memtable page format helpers.
 *
 * See page.h for the layout and conventions. This file is
 * intentionally callable from anywhere that has a Page pointer;
 * it does no buffer locking, WAL emission, or chain management.
 * Those concerns live one layer up (log.c).
 *
 * Also exposed here is bm25_test_memtable_page(text), a scaffold
 * SQL function used to exercise the helpers from the SQL
 * regression suite for unit-level coverage.
 */
#include <postgres.h>

#include <access/transam.h>
#include <fmgr.h>
#include <storage/block.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <utils/builtins.h>
#include <utils/memutils.h>

#include "constants.h"
#include "memtable/page.h"

/*
 * Page header is placed at PageGetContents(page), which is
 * MAXALIGN(SizeOfPageHeaderData) bytes from the start of the
 * page.  TP_MEMTABLE_PAGE_HEADER_OFFSET captures that.
 */
TpMemtablePageHeader *
tp_memtable_page_header(Page page)
{
	return (TpMemtablePageHeader *)((char *)page +
									TP_MEMTABLE_PAGE_HEADER_OFFSET);
}

void
tp_memtable_page_init(Page page)
{
	TpMemtablePageHeader *hdr;

	PageInit(page, BLCKSZ, 0);

	hdr				 = tp_memtable_page_header(page);
	hdr->magic		 = TP_MEMTABLE_PAGE_MAGIC;
	hdr->version	 = TP_MEMTABLE_PAGE_VERSION;
	hdr->flags		 = 0;
	hdr->n_records	 = 0;
	hdr->free_offset = (uint16)TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET;
	hdr->next_block	 = InvalidBlockNumber;
	hdr->dead_fxid	 = InvalidFullTransactionId;

	/*
	 * Mark the entire page as "in-use" so GenericXLog does not treat
	 * the area we use for records as the standard PD_LOWER..PD_UPPER
	 * hole.  GenericXLogFinish() zeroes that hole when applying the
	 * scratch image to the buffer, which would silently wipe out our
	 * header and records.  Setting pd_lower=BLCKSZ collapses the hole
	 * to zero bytes so the full page is included in the delta and
	 * the buffer write-back.  The standard PageHeader.pd_special and
	 * pd_upper remain at BLCKSZ from PageInit, which is fine: we do
	 * not use line pointers or PageAddItem on this page format, so
	 * the invariants those macros rely on are not exercised.
	 */
	((PageHeader)page)->pd_lower = BLCKSZ;
}

bool
tp_memtable_page_is_valid(Page page)
{
	TpMemtablePageHeader *hdr = tp_memtable_page_header(page);

	return hdr->magic == TP_MEMTABLE_PAGE_MAGIC &&
		   hdr->version == TP_MEMTABLE_PAGE_VERSION;
}

uint32
tp_memtable_record_size(uint32 vector_len)
{
	return (uint32)MAXALIGN(TP_MEMTABLE_RECORD_BASE_SIZE + vector_len);
}

bool
tp_memtable_page_can_fit(Page page, uint32 vector_len)
{
	TpMemtablePageHeader *hdr  = tp_memtable_page_header(page);
	uint32				  need = tp_memtable_record_size(vector_len);

	/*
	 * free_offset is always MAXALIGN'd; BLCKSZ is naturally
	 * MAXALIGN'd; therefore the comparison is exact (no rounding).
	 */
	return (uint32)hdr->free_offset + need <= (uint32)BLCKSZ;
}

TpMemtableRecord *
tp_memtable_page_append(
		Page		page,
		ItemPointer ctid,
		int32		doc_length,
		const char *vector_bytes,
		uint32		vector_len)
{
	TpMemtablePageHeader *hdr;
	TpMemtableRecord	 *rec;
	uint32				  record_size;

	hdr = tp_memtable_page_header(page);

	if (!tp_memtable_page_can_fit(page, vector_len))
		elog(ERROR,
			 "tp_memtable_page_append: record (vector_len=%u, total=%u) does "
			 "not fit on page (free_offset=%u, BLCKSZ=%d)",
			 vector_len,
			 tp_memtable_record_size(vector_len),
			 hdr->free_offset,
			 BLCKSZ);

	rec = (TpMemtableRecord *)((char *)page + hdr->free_offset);

	ItemPointerCopy(ctid, &rec->ctid);
	rec->flags		= 0;
	rec->doc_length = doc_length;
	rec->vector_len = vector_len;
	if (vector_len > 0)
		memcpy(rec->vector_bytes, vector_bytes, vector_len);

	record_size = tp_memtable_record_size(vector_len);
	hdr->free_offset += (uint16)record_size;
	hdr->n_records++;

	return rec;
}

TpMemtableRecord *
tp_memtable_page_first(Page page)
{
	TpMemtablePageHeader *hdr = tp_memtable_page_header(page);

	if (hdr->n_records == 0)
		return NULL;

	return (TpMemtableRecord *)((char *)page +
								TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET);
}

TpMemtableRecord *
tp_memtable_page_next(Page page, TpMemtableRecord *cur)
{
	TpMemtablePageHeader *hdr = tp_memtable_page_header(page);
	char				 *cur_end;
	char				 *next;

	cur_end = (char *)cur + tp_memtable_record_size(cur->vector_len);
	next	= (char *)page + hdr->free_offset;

	if (cur_end >= next)
		return NULL;

	return (TpMemtableRecord *)cur_end;
}

BlockNumber
tp_memtable_page_get_next(Page page)
{
	return tp_memtable_page_header(page)->next_block;
}

void
tp_memtable_page_set_next(Page page, BlockNumber blk)
{
	tp_memtable_page_header(page)->next_block = blk;
}

uint16
tp_memtable_page_n_records(Page page)
{
	return tp_memtable_page_header(page)->n_records;
}

/* ---------------------------------------------------------------------
 * Multi-page (fragment) record support.
 *
 * Layout of a FRAGMENT head page:
 *   header (flags=0, n_records=1, next_block=C1)
 *   one TpMemtableRecord with flags=FRAGMENT, vector_len=FULL,
 *   inline prefix bytes that fill the rest of the page.
 *   The page's free_offset advances to BLCKSZ; nothing else
 *   appends to this page.
 *
 * Layout of a continuation page:
 *   header (flags=CONTINUATION, n_records=0, next_block=Cnext|Tnew|Invalid)
 *   raw payload bytes at [first_record_offset, free_offset).
 *   No record framing on this page.
 *
 * The chain walker, on encountering a FRAGMENT head, reassembles
 * the full payload by reading inline bytes + each continuation's
 * chunk in order, then skips past the last continuation when
 * resuming the outer walk.
 * ---------------------------------------------------------------------
 */

uint32
tp_memtable_fragment_inline_capacity_max(void)
{
	/*
	 * The head page starts empty (free_offset =
	 * TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET).  After appending a
	 * FRAGMENT head record with inline payload `inline_len`, the
	 * page's free_offset must reach BLCKSZ exactly (under
	 * MAXALIGN).  The record total size is
	 * MAXALIGN(base + inline_len), so we want the largest
	 * `inline_len` such that
	 *   first_record_offset + MAXALIGN(base + inline_len) <= BLCKSZ
	 *
	 * Since first_record_offset and BLCKSZ are MAXALIGN-multiples,
	 * MAXALIGN(base + inline_len) <= BLCKSZ - first_record_offset.
	 * Solving: inline_len <= BLCKSZ - first_record_offset - base
	 * once we account for the MAXALIGN.  The largest aligned
	 * record total that fits is BLCKSZ - first_record_offset, so:
	 */
	return (uint32)((BLCKSZ - TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET) -
					TP_MEMTABLE_RECORD_BASE_SIZE);
}

TpMemtableRecord *
tp_memtable_page_append_fragment_head(
		Page		page,
		ItemPointer ctid,
		int32		doc_length,
		const char *prefix,
		uint32		prefix_len,
		uint32		total_vector_len)
{
	TpMemtablePageHeader *hdr;
	TpMemtableRecord	 *rec;
	uint32				  expected_prefix;

	hdr				= tp_memtable_page_header(page);
	expected_prefix = tp_memtable_fragment_inline_capacity_max();

	if (prefix_len != expected_prefix)
		elog(ERROR,
			 "tp_memtable_page_append_fragment_head: prefix_len=%u "
			 "must equal inline capacity %u",
			 prefix_len,
			 expected_prefix);

	if (total_vector_len <= prefix_len)
		elog(ERROR,
			 "tp_memtable_page_append_fragment_head: total_vector_len=%u "
			 "must exceed prefix_len=%u",
			 total_vector_len,
			 prefix_len);

	if (hdr->n_records != 0 ||
		hdr->free_offset != TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET)
		elog(ERROR,
			 "tp_memtable_page_append_fragment_head: head page must be "
			 "empty (n_records=%u, free_offset=%u)",
			 hdr->n_records,
			 hdr->free_offset);

	rec = (TpMemtableRecord *)((char *)page + hdr->free_offset);

	ItemPointerCopy(ctid, &rec->ctid);
	rec->flags		= TP_MEMTABLE_RECORD_FLAG_FRAGMENT;
	rec->doc_length = doc_length;
	rec->vector_len = total_vector_len;
	if (prefix_len > 0)
		memcpy(rec->vector_bytes, prefix, prefix_len);

	/*
	 * Advance free_offset to BLCKSZ.  The inline payload
	 * occupies bytes [vector_bytes_offset, BLCKSZ); since
	 * prefix_len == BLCKSZ - first_record_offset - base (the
	 * formula above), free_offset = first_record_offset + base
	 * + prefix_len = BLCKSZ exactly.  Sanity-asserted via the
	 * arithmetic.
	 */
	hdr->free_offset = (uint16)BLCKSZ;
	hdr->n_records	 = 1;

	return rec;
}

uint32
tp_memtable_continuation_max_chunk(void)
{
	/*
	 * Continuation pages hold a single contiguous chunk of
	 * payload bytes at [first_record_offset, BLCKSZ).  No
	 * record framing; the chunk's length is implicit in the
	 * page's free_offset.
	 */
	return (uint32)(BLCKSZ - TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET);
}

void
tp_memtable_continuation_page_init(
		Page page, const char *chunk, uint32 chunk_len, BlockNumber next_block)
{
	TpMemtablePageHeader *hdr;
	uint32				  cap = tp_memtable_continuation_max_chunk();

	if (chunk_len > cap)
		elog(ERROR,
			 "tp_memtable_continuation_page_init: chunk_len %u exceeds "
			 "capacity %u",
			 chunk_len,
			 cap);

	PageInit(page, BLCKSZ, 0);

	hdr				 = tp_memtable_page_header(page);
	hdr->magic		 = TP_MEMTABLE_PAGE_MAGIC;
	hdr->version	 = TP_MEMTABLE_PAGE_VERSION;
	hdr->flags		 = TP_MEMTABLE_PAGE_FLAG_CONTINUATION;
	hdr->n_records	 = 0;
	hdr->free_offset = (uint16)(TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET +
								chunk_len);
	hdr->next_block	 = next_block;
	hdr->dead_fxid	 = InvalidFullTransactionId;

	if (chunk_len > 0)
		memcpy((char *)page + TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET,
			   chunk,
			   chunk_len);

	/*
	 * Suppress the GenericXLog hole between pd_lower and pd_upper
	 * for the same reason as regular memtable pages — see
	 * tp_memtable_page_init().
	 */
	((PageHeader)page)->pd_lower = BLCKSZ;
}

bool
tp_memtable_page_is_continuation(Page page)
{
	TpMemtablePageHeader *hdr = tp_memtable_page_header(page);

	return tp_memtable_page_is_valid(page) &&
		   (hdr->flags & TP_MEMTABLE_PAGE_FLAG_CONTINUATION) != 0;
}

const char *
tp_memtable_continuation_page_chunk(Page page, uint32 *out_len)
{
	TpMemtablePageHeader *hdr = tp_memtable_page_header(page);

	Assert((hdr->flags & TP_MEMTABLE_PAGE_FLAG_CONTINUATION) != 0);
	*out_len = (uint32)hdr->free_offset - TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET;
	return (const char *)page + TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET;
}

/*
 * Set DEAD + dead_fxid on a GenericXLog working copy.  Idempotent:
 * no-op if the page is already marked dead (first horizon wins).
 */
void
tp_memtable_page_mark_dead(Page page, FullTransactionId horizon)
{
	TpMemtablePageHeader *hdr = tp_memtable_page_header(page);

	if (!tp_memtable_page_is_valid(page))
		return;

	if (tp_memtable_page_is_dead(page))
		return;

	hdr->flags |= TP_MEMTABLE_PAGE_FLAG_DEAD;
	hdr->dead_fxid = horizon;
}

bool
tp_memtable_page_is_dead(Page page)
{
	TpMemtablePageHeader *hdr = tp_memtable_page_header(page);
	return tp_memtable_page_is_valid(page) &&
		   (hdr->flags & TP_MEMTABLE_PAGE_FLAG_DEAD) != 0;
}

/* ---------------------------------------------------------------------
 * Scaffold SQL function for unit-level coverage.
 *
 * bm25_test_memtable_page(case_name text) -> text
 *
 * Returns 'OK' if `case_name` passes, or 'FAIL: <detail>' otherwise.
 * Cases exercise the helpers above against a scratch BLCKSZ buffer;
 * no shared buffers, no WAL, no relation involvement.  See
 * test/sql/memtable_page.sql for the per-case asserts.
 * ---------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(bm25_test_memtable_page);

#define TEST_FAIL(fmt, ...)                                 \
	do                                                      \
	{                                                       \
		char *_buf = psprintf("FAIL: " fmt, ##__VA_ARGS__); \
		PG_RETURN_TEXT_P(cstring_to_text(_buf));            \
	} while (0)

#define TEST_OK() PG_RETURN_TEXT_P(cstring_to_text("OK"))

static Page
test_alloc_page(void)
{
	/*
	 * BLCKSZ is at most 32 KB and palloc returns MAXALIGN'd memory.
	 * Zero-fill so the "reject_uninitialized" case sees a page that
	 * deliberately does not carry our magic.
	 */
	Page p = (Page)palloc0(BLCKSZ);
	return p;
}

static ItemPointerData
test_make_ctid(BlockNumber blk, OffsetNumber off)
{
	ItemPointerData itp;
	ItemPointerSet(&itp, blk, off);
	return itp;
}

Datum
bm25_test_memtable_page(PG_FUNCTION_ARGS)
{
	text *case_text = PG_GETARG_TEXT_PP(0);
	char *case_name = text_to_cstring(case_text);

	if (strcmp(case_name, "reject_uninitialized") == 0)
	{
		Page p = test_alloc_page();
		if (tp_memtable_page_is_valid(p))
			TEST_FAIL("uninitialized (zero) page reported valid");
		TEST_OK();
	}
	else if (strcmp(case_name, "init_empty") == 0)
	{
		Page				  p = test_alloc_page();
		TpMemtablePageHeader *hdr;

		tp_memtable_page_init(p);
		if (!tp_memtable_page_is_valid(p))
			TEST_FAIL("init produced an invalid page");
		hdr = tp_memtable_page_header(p);
		if (hdr->magic != TP_MEMTABLE_PAGE_MAGIC)
			TEST_FAIL("unexpected magic 0x%x", hdr->magic);
		if (hdr->version != TP_MEMTABLE_PAGE_VERSION)
			TEST_FAIL("unexpected version %u", hdr->version);
		if (hdr->n_records != 0)
			TEST_FAIL("n_records=%u, expected 0", hdr->n_records);
		if (hdr->flags != 0)
			TEST_FAIL("flags=%u, expected 0", hdr->flags);
		if (hdr->free_offset != TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET)
			TEST_FAIL(
					"free_offset=%u, expected %u",
					hdr->free_offset,
					(unsigned)TP_MEMTABLE_PAGE_FIRST_RECORD_OFFSET);
		if (hdr->next_block != InvalidBlockNumber)
			TEST_FAIL(
					"next_block=%u, expected InvalidBlockNumber",
					hdr->next_block);
		if (tp_memtable_page_n_records(p) != 0)
			TEST_FAIL(
					"n_records accessor returned %u, expected 0",
					tp_memtable_page_n_records(p));
		TEST_OK();
	}
	else if (strcmp(case_name, "iterate_empty") == 0)
	{
		Page p = test_alloc_page();
		tp_memtable_page_init(p);
		if (tp_memtable_page_first(p) != NULL)
			TEST_FAIL("first(empty) returned non-NULL");
		TEST_OK();
	}
	else if (strcmp(case_name, "one_record_roundtrip") == 0)
	{
		Page			  p		  = test_alloc_page();
		ItemPointerData	  ctid	  = test_make_ctid(42, 7);
		const char		 *vec	  = "\x01\x02\x03\x04\x05\x06\x07";
		uint32			  vec_len = 7;
		TpMemtableRecord *rec;
		TpMemtableRecord *iter;

		tp_memtable_page_init(p);
		if (!tp_memtable_page_can_fit(p, vec_len))
			TEST_FAIL("fresh page cannot fit a 7-byte record");
		rec = tp_memtable_page_append(p, &ctid, 12345, vec, vec_len);
		if (rec == NULL)
			TEST_FAIL("append returned NULL");
		if (tp_memtable_page_n_records(p) != 1)
			TEST_FAIL(
					"n_records=%u after one append",
					tp_memtable_page_n_records(p));

		iter = tp_memtable_page_first(p);
		if (iter == NULL)
			TEST_FAIL("first() returned NULL after one append");
		if (iter != rec)
			TEST_FAIL("first() differs from append() pointer");
		if (!ItemPointerEquals(&iter->ctid, &ctid))
			TEST_FAIL(
					"ctid roundtrip mismatch: got (%u,%u)",
					ItemPointerGetBlockNumber(&iter->ctid),
					ItemPointerGetOffsetNumber(&iter->ctid));
		if (iter->doc_length != 12345)
			TEST_FAIL("doc_length=%d, expected 12345", iter->doc_length);
		if (iter->vector_len != vec_len)
			TEST_FAIL("vector_len=%u, expected %u", iter->vector_len, vec_len);
		if (memcmp(iter->vector_bytes, vec, vec_len) != 0)
			TEST_FAIL("vector_bytes mismatch");
		if (tp_memtable_page_next(p, iter) != NULL)
			TEST_FAIL("next() of last record returned non-NULL");
		TEST_OK();
	}
	else if (strcmp(case_name, "many_records_roundtrip") == 0)
	{
		Page			  p = test_alloc_page();
		const int		  N = 10;
		int				  i;
		TpMemtableRecord *iter;
		char			  vec[64];

		tp_memtable_page_init(p);

		for (i = 0; i < N; i++)
		{
			ItemPointerData ctid = test_make_ctid(
					(BlockNumber)(1000 + i), (OffsetNumber)(i + 1));
			uint32 vec_len = (uint32)(1 + (i % 17));
			int	   j;
			for (j = 0; j < (int)vec_len; j++)
				vec[j] = (char)((i * 31 + j) & 0xff);

			if (!tp_memtable_page_can_fit(p, vec_len))
				TEST_FAIL(
						"can_fit returned false at i=%d, vec_len=%u",
						i,
						vec_len);
			tp_memtable_page_append(p, &ctid, 100 + i, vec, vec_len);
		}

		if (tp_memtable_page_n_records(p) != N)
			TEST_FAIL(
					"n_records=%u after %d appends",
					tp_memtable_page_n_records(p),
					N);

		iter = tp_memtable_page_first(p);
		for (i = 0; i < N; i++)
		{
			ItemPointerData expected_ctid = test_make_ctid(
					(BlockNumber)(1000 + i), (OffsetNumber)(i + 1));
			uint32 expected_vec_len = (uint32)(1 + (i % 17));
			int	   j;

			if (iter == NULL)
				TEST_FAIL(
						"iterator exhausted at i=%d, expected %d records",
						i,
						N);
			if (!ItemPointerEquals(&iter->ctid, &expected_ctid))
				TEST_FAIL(
						"ctid mismatch at i=%d: got (%u,%u)",
						i,
						ItemPointerGetBlockNumber(&iter->ctid),
						ItemPointerGetOffsetNumber(&iter->ctid));
			if (iter->doc_length != 100 + i)
				TEST_FAIL(
						"doc_length mismatch at i=%d: got %d",
						i,
						iter->doc_length);
			if (iter->vector_len != expected_vec_len)
				TEST_FAIL(
						"vector_len mismatch at i=%d: got %u",
						i,
						iter->vector_len);
			for (j = 0; j < (int)expected_vec_len; j++)
			{
				char expected = (char)((i * 31 + j) & 0xff);
				if (iter->vector_bytes[j] != expected)
					TEST_FAIL("vector_bytes mismatch at i=%d, j=%d", i, j);
			}
			iter = tp_memtable_page_next(p, iter);
		}
		if (iter != NULL)
			TEST_FAIL("iterator did not terminate after %d records", N);
		TEST_OK();
	}
	else if (strcmp(case_name, "can_fit_boundary") == 0)
	{
		Page				  p = test_alloc_page();
		TpMemtablePageHeader *hdr;
		uint32				  remaining;

		tp_memtable_page_init(p);
		hdr		  = tp_memtable_page_header(p);
		remaining = BLCKSZ - hdr->free_offset;

		/*
		 * A record requiring `remaining` bytes total (header +
		 * payload + padding) must fit; one byte more must not.
		 */
		if (remaining < TP_MEMTABLE_RECORD_BASE_SIZE)
			TEST_FAIL("remaining=%u less than record base size", remaining);

		/*
		 * Largest vector_len that yields total <= remaining.
		 * Total = MAXALIGN(base + vec).  Find the largest vec that
		 * still fits.
		 */
		{
			uint32 max_vec = remaining - TP_MEMTABLE_RECORD_BASE_SIZE;
			uint32 too_big = max_vec + (uint32)MAXIMUM_ALIGNOF + 1;

			if (!tp_memtable_page_can_fit(p, max_vec))
				TEST_FAIL("can_fit(max_vec=%u) returned false", max_vec);
			if (tp_memtable_page_can_fit(p, too_big))
				TEST_FAIL("can_fit(too_big=%u) returned true", too_big);
		}
		TEST_OK();
	}
	else if (strcmp(case_name, "fill_until_full") == 0)
	{
		Page			  p		   = test_alloc_page();
		int				  appended = 0;
		uint32			  vec_len  = 32;
		char			  vec[64];
		TpMemtableRecord *iter;
		int				  i;

		for (i = 0; i < (int)vec_len; i++)
			vec[i] = (char)i;

		tp_memtable_page_init(p);
		while (tp_memtable_page_can_fit(p, vec_len))
		{
			ItemPointerData ctid = test_make_ctid(
					(BlockNumber)appended, (OffsetNumber)(appended + 1));
			tp_memtable_page_append(p, &ctid, appended, vec, vec_len);
			appended++;
		}

		if (appended == 0)
			TEST_FAIL("appended zero records (page too small?)");
		if (tp_memtable_page_n_records(p) != appended)
			TEST_FAIL(
					"n_records=%u, appended=%d",
					tp_memtable_page_n_records(p),
					appended);

		iter = tp_memtable_page_first(p);
		for (i = 0; i < appended; i++)
		{
			if (iter == NULL)
				TEST_FAIL("iterator NULL at i=%d", i);
			if (iter->doc_length != i)
				TEST_FAIL("doc_length=%d, expected %d", iter->doc_length, i);
			iter = tp_memtable_page_next(p, iter);
		}
		if (iter != NULL)
			TEST_FAIL("iterator did not terminate");
		TEST_OK();
	}
	else if (strcmp(case_name, "next_block_roundtrip") == 0)
	{
		Page p = test_alloc_page();
		tp_memtable_page_init(p);
		if (tp_memtable_page_get_next(p) != InvalidBlockNumber)
			TEST_FAIL(
					"fresh page next_block=%u, expected Invalid",
					tp_memtable_page_get_next(p));
		tp_memtable_page_set_next(p, 12345);
		if (tp_memtable_page_get_next(p) != 12345)
			TEST_FAIL(
					"after set, next_block=%u, expected 12345",
					tp_memtable_page_get_next(p));
		tp_memtable_page_set_next(p, InvalidBlockNumber);
		if (tp_memtable_page_get_next(p) != InvalidBlockNumber)
			TEST_FAIL(
					"after clear, next_block=%u, expected Invalid",
					tp_memtable_page_get_next(p));
		TEST_OK();
	}

	TEST_FAIL("unknown case '%s'", case_name);
}
