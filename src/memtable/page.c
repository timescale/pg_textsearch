/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * page.c - On-disk memtable page format helpers.
 *
 * See page.h for the layout and conventions. This file is
 * intentionally callable from anywhere that has a Page pointer;
 * it does no buffer locking, WAL emission, or chain management.
 * Those concerns live one layer up (Phase 2+).
 *
 * Also exposed here is bm25_test_memtable_page(text), a scaffold
 * SQL function used to exercise the helpers from the SQL
 * regression suite while the rest of the redesign is in flight.
 * It is internal-only and may be removed once the integration
 * tests cover the same paths end-to-end.
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
	rec->_pad		= 0;
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
