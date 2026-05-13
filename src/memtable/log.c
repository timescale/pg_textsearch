/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * log.c - On-disk memtable write path.
 *
 * Phase 2 of the memtable v2 redesign (issue #374).  See log.h
 * for the concurrency contract and high-level semantics.
 *
 * Scaffold SQL functions are exposed at the bottom of this file:
 *
 *   bm25_test_memtable_append(index_name text, case_name text)
 *       → text         scenario harness, returns 'OK' or 'FAIL: …'
 *
 *   bm25_memtable_chain(index_name text)
 *       → SETOF (blkno bigint, n_records int, free_offset int,
 *                next_block bigint)
 *
 * These functions are internal-only scaffolding and will be
 * retired once the integration tests in later phases cover the
 * same paths end-to-end.
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <access/relation.h>
#include <catalog/index.h>
#include <fmgr.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <storage/block.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/itemptr.h>
#include <storage/lwlock.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/relcache.h>
#include <utils/varlena.h>

#include "constants.h"
#include "index/metapage.h"
#include "index/resolve.h"
#include "index/state.h"
#include "memtable/log.h"
#include "memtable/page.h"

/*
 * Read meta.memtable_tail_blkno under SHARED lock on the
 * metapage buffer, then release.  Returns InvalidBlockNumber if
 * the chain is empty.  The release-before-page-lock pattern is
 * the reader half of the lock-order contract documented in
 * log.h.
 */
static BlockNumber
memtable_read_tail_blkno(Relation rel)
{
	Buffer			buf;
	Page			page;
	TpIndexMetaPage metap;
	BlockNumber		tail;

	buf = ReadBuffer(rel, TP_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page  = BufferGetPage(buf);
	metap = (TpIndexMetaPage)PageGetContents(page);
	tail  = metap->memtable_tail_blkno;
	UnlockReleaseBuffer(buf);

	return tail;
}

/*
 * Bootstrap path: the chain is empty.  Allocate the first page,
 * append the record, and update meta.{head,tail}_blkno — all
 * atomically inside one GenericXLog covering {meta, new page}.
 *
 * Re-checks meta.tail_blkno under EXCLUSIVE meta lock to detect
 * the lost-race case where another backend bootstrapped first.
 * On re-check failure we release everything and return
 * InvalidBlockNumber so the caller can retry via the normal
 * append path.
 */
static BlockNumber
memtable_bootstrap_and_append(
		Relation	rel,
		ItemPointer ctid,
		int32		doc_length,
		const char *vector_bytes,
		uint32		vector_len)
{
	Buffer			  metabuf;
	Buffer			  newbuf;
	BlockNumber		  newblk;
	Page			  metapage_local;
	Page			  newpage_local;
	TpIndexMetaPage	  metap;
	GenericXLogState *xlog_state;

	metabuf = ReadBuffer(rel, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	{
		Page			cur_meta_page = BufferGetPage(metabuf);
		TpIndexMetaPage cur_metap	  = (TpIndexMetaPage)PageGetContents(
				cur_meta_page);

		if (cur_metap->memtable_tail_blkno != InvalidBlockNumber)
		{
			/* Lost the race: another backend bootstrapped first. */
			UnlockReleaseBuffer(metabuf);
			return InvalidBlockNumber;
		}
	}

	newbuf =
			ExtendBufferedRel(BMR_REL(rel), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
	newblk = BufferGetBlockNumber(newbuf);

	xlog_state	   = GenericXLogStart(rel);
	metapage_local = GenericXLogRegisterBuffer(xlog_state, metabuf, 0);
	newpage_local  = GenericXLogRegisterBuffer(
			 xlog_state, newbuf, GENERIC_XLOG_FULL_IMAGE);

	tp_memtable_page_init(newpage_local);
	tp_memtable_page_append(
			newpage_local, ctid, doc_length, vector_bytes, vector_len);

	metap = (TpIndexMetaPage)PageGetContents(metapage_local);
	metap->memtable_head_blkno = newblk;
	metap->memtable_tail_blkno = newblk;

	GenericXLogFinish(xlog_state);

	UnlockReleaseBuffer(newbuf);
	UnlockReleaseBuffer(metabuf);

	return newblk;
}

/*
 * Extend path: tail is full.  Allocate a new tail page, link
 * old tail → new, append the record on the new page, and
 * update meta.tail_blkno.  All inside one GenericXLog covering
 * {old tail, new page, meta} (3 of 4 GenericXLog buffer slots).
 *
 * Caller passes the old tail buffer already locked EXCLUSIVE
 * and verified valid + full.
 */
static BlockNumber
memtable_extend_and_append(
		Relation	rel,
		Buffer		tailbuf,
		ItemPointer ctid,
		int32		doc_length,
		const char *vector_bytes,
		uint32		vector_len)
{
	Buffer			  newbuf;
	Buffer			  metabuf;
	BlockNumber		  newblk;
	Page			  tailpage_local;
	Page			  newpage_local;
	Page			  metapage_local;
	TpIndexMetaPage	  metap;
	GenericXLogState *xlog_state;

	newbuf =
			ExtendBufferedRel(BMR_REL(rel), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
	newblk = BufferGetBlockNumber(newbuf);

	metabuf = ReadBuffer(rel, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	xlog_state	   = GenericXLogStart(rel);
	tailpage_local = GenericXLogRegisterBuffer(xlog_state, tailbuf, 0);
	newpage_local  = GenericXLogRegisterBuffer(
			 xlog_state, newbuf, GENERIC_XLOG_FULL_IMAGE);
	metapage_local = GenericXLogRegisterBuffer(xlog_state, metabuf, 0);

	tp_memtable_page_init(newpage_local);
	tp_memtable_page_append(
			newpage_local, ctid, doc_length, vector_bytes, vector_len);

	tp_memtable_page_set_next(tailpage_local, newblk);

	metap = (TpIndexMetaPage)PageGetContents(metapage_local);
	metap->memtable_tail_blkno = newblk;

	GenericXLogFinish(xlog_state);

	UnlockReleaseBuffer(metabuf);
	UnlockReleaseBuffer(newbuf);

	return newblk;
}

BlockNumber
tp_memtable_append(
		Relation	rel,
		ItemPointer ctid,
		int32		doc_length,
		const char *vector_bytes,
		uint32		vector_len)
{
	TpLocalIndexState *index_state;
	Oid				   index_oid;

	Assert(rel != NULL);
	Assert(ctid != NULL);
	Assert(vector_len == 0 || vector_bytes != NULL);

	/*
	 * Up-front size guard.  Reject records whose vector payload
	 * cannot fit on an otherwise-empty page before any buffer
	 * or WAL work — otherwise we would leak a relation
	 * extension or have to ereport from inside a critical
	 * section.
	 */
	if (vector_len > TP_MEMTABLE_PAGE_MAX_VECTOR_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("pg_textsearch memtable record too large: vector_len "
						"%u exceeds maximum %u",
						vector_len,
						(unsigned)TP_MEMTABLE_PAGE_MAX_VECTOR_LEN)));

	/*
	 * Acquire the per-index LWLock in SHARED mode.  In Phase 2
	 * no LW_EXCLUSIVE holder exists (spill still mutates the
	 * DSA memtable, not the on-disk one).  Taking the lock now
	 * locks in the discipline so the Phase 4 switchover does
	 * not churn this code's signature.
	 */
	index_oid	= RelationGetRelid(rel);
	index_state = tp_get_local_index_state(index_oid);
	if (index_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_textsearch index state not available for \"%s\"",
						RelationGetRelationName(rel))));

	tp_acquire_index_lock(index_state, LW_SHARED);

	/*
	 * Fast / extend / bootstrap retry loop.  Each retry is
	 * caused by another backend either bootstrapping or
	 * extending the chain; under bounded concurrency the loop
	 * terminates.  We bound the iteration count defensively
	 * with INT_MAX/2 just to surface any pathological live-lock
	 * during development.
	 */
	for (int iter = 0; iter < INT_MAX / 2; iter++)
	{
		BlockNumber tail_blkno;
		Buffer		tailbuf;
		Page		tailpage;
		BlockNumber result;

		CHECK_FOR_INTERRUPTS();

		tail_blkno = memtable_read_tail_blkno(rel);

		if (tail_blkno == InvalidBlockNumber)
		{
			result = memtable_bootstrap_and_append(
					rel, ctid, doc_length, vector_bytes, vector_len);
			if (result != InvalidBlockNumber)
				return result;

			/* Lost the bootstrap race; retry as an appender. */
			continue;
		}

		tailbuf = ReadBuffer(rel, tail_blkno);
		LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);
		tailpage = BufferGetPage(tailbuf);

		if (!tp_memtable_page_is_valid(tailpage))
		{
			UnlockReleaseBuffer(tailbuf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch memtable tail page at block "
							"%u in index \"%s\" is not a valid memtable "
							"page (magic mismatch)",
							tail_blkno,
							RelationGetRelationName(rel))));
		}

		if (tp_memtable_page_get_next(tailpage) != InvalidBlockNumber)
		{
			/*
			 * Stale tail: another backend extended the chain
			 * after our metapage read but before our tail
			 * lock.  Release and re-read metapage to find the
			 * new tail.  Each retry observes one completed
			 * extend by another backend.
			 */
			UnlockReleaseBuffer(tailbuf);
			continue;
		}

		if (tp_memtable_page_can_fit(tailpage, vector_len))
		{
			GenericXLogState *xlog_state;
			Page			  tailpage_local;

			xlog_state	   = GenericXLogStart(rel);
			tailpage_local = GenericXLogRegisterBuffer(xlog_state, tailbuf, 0);

			tp_memtable_page_append(
					tailpage_local,
					ctid,
					doc_length,
					vector_bytes,
					vector_len);

			GenericXLogFinish(xlog_state);
			UnlockReleaseBuffer(tailbuf);
			return tail_blkno;
		}

		/* Tail is full; allocate a new page and append there. */
		result = memtable_extend_and_append(
				rel, tailbuf, ctid, doc_length, vector_bytes, vector_len);
		UnlockReleaseBuffer(tailbuf);
		return result;
	}

	/* Unreachable under bounded concurrency. */
	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("pg_textsearch memtable append did not converge for "
					"index \"%s\"",
					RelationGetRelationName(rel))));
	return InvalidBlockNumber; /* keep compilers happy */
}

/* ---------------------------------------------------------------------
 * Scaffold SQL functions.  Internal-only; subject to removal once
 * Phase 4+ provides end-to-end coverage.
 * ---------------------------------------------------------------------
 */

/*
 * Open the named pg_textsearch index by name, taking
 * RowExclusiveLock so concurrent DDL is excluded but normal
 * inserts/scans are not.  Errors if the name does not resolve.
 */
static Relation
test_open_index(const char *index_name)
{
	Oid oid;

	oid = tp_resolve_index_name_shared(index_name);
	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("pg_textsearch index \"%s\" not found", index_name)));

	return index_open(oid, RowExclusiveLock);
}

/*
 * Deterministic synthetic vector payload, generated from the
 * caller's seed so two calls with the same arguments produce
 * the same bytes.  Used by the scaffold cases below.
 */
static void
test_fill_vector(char *buf, uint32 len, uint32 seed)
{
	for (uint32 i = 0; i < len; i++)
		buf[i] = (char)((seed * 1664525u + i * 1013904223u) & 0xff);
}

#define TEST_FAIL(fmt, ...)                                 \
	do                                                      \
	{                                                       \
		char *_buf = psprintf("FAIL: " fmt, ##__VA_ARGS__); \
		index_close(rel, RowExclusiveLock);                 \
		PG_RETURN_TEXT_P(cstring_to_text(_buf));            \
	} while (0)

#define TEST_OK()                                \
	do                                           \
	{                                            \
		index_close(rel, RowExclusiveLock);      \
		PG_RETURN_TEXT_P(cstring_to_text("OK")); \
	} while (0)

static uint32
test_chain_length(Relation rel)
{
	TpIndexMetaPage metap = tp_get_metapage(rel);
	BlockNumber		cur	  = metap->memtable_head_blkno;
	uint32			count = 0;

	pfree(metap);

	while (cur != InvalidBlockNumber)
	{
		Buffer		buf;
		Page		page;
		BlockNumber next;

		buf = ReadBuffer(rel, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!tp_memtable_page_is_valid(page))
		{
			UnlockReleaseBuffer(buf);
			elog(ERROR,
				 "test_chain_length: invalid memtable page at block %u",
				 cur);
		}
		next = tp_memtable_page_get_next(page);
		UnlockReleaseBuffer(buf);

		count++;
		cur = next;
	}

	return count;
}

PG_FUNCTION_INFO_V1(bm25_test_memtable_append);

Datum
bm25_test_memtable_append(PG_FUNCTION_ARGS)
{
	text	*idx_text  = PG_GETARG_TEXT_PP(0);
	text	*case_text = PG_GETARG_TEXT_PP(1);
	char	*idx_name  = text_to_cstring(idx_text);
	char	*case_name = text_to_cstring(case_text);
	Relation rel	   = test_open_index(idx_name);

	if (strcmp(case_name, "empty_index") == 0)
	{
		TpIndexMetaPage metap = tp_get_metapage(rel);

		if (metap->memtable_head_blkno != InvalidBlockNumber)
			TEST_FAIL(
					"empty index has head_blkno=%u, expected "
					"InvalidBlockNumber",
					metap->memtable_head_blkno);
		if (metap->memtable_tail_blkno != InvalidBlockNumber)
			TEST_FAIL(
					"empty index has tail_blkno=%u, expected "
					"InvalidBlockNumber",
					metap->memtable_tail_blkno);
		pfree(metap);
		TEST_OK();
	}
	else if (strcmp(case_name, "single_append") == 0)
	{
		ItemPointerData ctid;
		char			vec[64];
		TpIndexMetaPage metap;
		BlockNumber		blk;

		ItemPointerSet(&ctid, 100, 1);
		test_fill_vector(vec, sizeof(vec), 1);

		blk = tp_memtable_append(rel, &ctid, 12345, vec, sizeof(vec));
		if (blk == InvalidBlockNumber)
			TEST_FAIL("single append returned InvalidBlockNumber");

		metap = tp_get_metapage(rel);
		if (metap->memtable_head_blkno != blk)
			TEST_FAIL(
					"head_blkno=%u, expected %u",
					metap->memtable_head_blkno,
					blk);
		if (metap->memtable_tail_blkno != blk)
			TEST_FAIL(
					"tail_blkno=%u, expected %u",
					metap->memtable_tail_blkno,
					blk);
		pfree(metap);

		if (test_chain_length(rel) != 1)
			TEST_FAIL("chain length != 1 after single append");
		TEST_OK();
	}
	else if (strcmp(case_name, "fits_on_one_page") == 0)
	{
		TpIndexMetaPage metap;
		const int		N = 10;
		char			vec[32];

		for (int i = 0; i < N; i++)
		{
			ItemPointerData ctid;
			ItemPointerSet(
					&ctid, (BlockNumber)(200 + i), (OffsetNumber)(i + 1));
			test_fill_vector(vec, sizeof(vec), (uint32)(i + 7));
			tp_memtable_append(rel, &ctid, 100 + i, vec, sizeof(vec));
		}

		metap = tp_get_metapage(rel);
		if (metap->memtable_head_blkno != metap->memtable_tail_blkno)
			TEST_FAIL(
					"head=%u tail=%u differ after small inserts",
					metap->memtable_head_blkno,
					metap->memtable_tail_blkno);
		pfree(metap);

		if (test_chain_length(rel) != 1)
			TEST_FAIL("chain length != 1 after %d small inserts", N);
		TEST_OK();
	}
	else if (strcmp(case_name, "extends_chain") == 0)
	{
		/*
		 * Each record is large enough that ~5 fill one page.
		 * Insert enough to force at least two pages.
		 */
		char	   *vec;
		uint32		vec_len = 1500;
		const int	N		= 20;
		uint32		chain_len;
		BlockNumber head_blkno;

		vec = palloc(vec_len);
		test_fill_vector(vec, vec_len, 42);

		for (int i = 0; i < N; i++)
		{
			ItemPointerData ctid;
			ItemPointerSet(
					&ctid, (BlockNumber)(300 + i), (OffsetNumber)(i + 1));
			tp_memtable_append(rel, &ctid, 500 + i, vec, vec_len);
		}
		pfree(vec);

		chain_len = test_chain_length(rel);
		if (chain_len < 2)
			TEST_FAIL("chain length %u, expected >= 2", chain_len);

		{
			TpIndexMetaPage metap = tp_get_metapage(rel);
			head_blkno			  = metap->memtable_head_blkno;
			if (metap->memtable_head_blkno == metap->memtable_tail_blkno)
				TEST_FAIL(
						"head_blkno=tail_blkno=%u after %d big inserts",
						metap->memtable_head_blkno,
						N);
			pfree(metap);
		}

		/* One more insert: head_blkno should not change. */
		{
			ItemPointerData ctid;
			char			small_vec[16];
			TpIndexMetaPage metap;

			ItemPointerSet(&ctid, 999, 1);
			test_fill_vector(small_vec, sizeof(small_vec), 99);
			tp_memtable_append(rel, &ctid, 777, small_vec, sizeof(small_vec));

			metap = tp_get_metapage(rel);
			if (metap->memtable_head_blkno != head_blkno)
				TEST_FAIL(
						"head_blkno changed from %u to %u",
						head_blkno,
						metap->memtable_head_blkno);
			pfree(metap);
		}
		TEST_OK();
	}
	else if (strcmp(case_name, "roundtrip") == 0)
	{
		/*
		 * Append a handful of records and read them back via a
		 * chain scan, verifying ctid/doc_length/vector bytes.
		 */
		const int		N = 7;
		char			vec[64];
		uint32			vec_len = 64;
		TpIndexMetaPage metap;
		BlockNumber		cur;
		int				idx;

		for (int i = 0; i < N; i++)
		{
			ItemPointerData ctid;
			ItemPointerSet(
					&ctid, (BlockNumber)(400 + i), (OffsetNumber)(i + 1));
			test_fill_vector(vec, vec_len, (uint32)(i * 17 + 3));
			tp_memtable_append(rel, &ctid, 800 + i, vec, vec_len);
		}

		metap = tp_get_metapage(rel);
		cur	  = metap->memtable_head_blkno;
		pfree(metap);

		idx = 0;
		while (cur != InvalidBlockNumber)
		{
			Buffer			  buf;
			Page			  page;
			TpMemtableRecord *r;
			BlockNumber		  next_blk;

			buf = ReadBuffer(rel, cur);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!tp_memtable_page_is_valid(page))
			{
				UnlockReleaseBuffer(buf);
				TEST_FAIL("invalid page at block %u", cur);
			}
			next_blk = tp_memtable_page_get_next(page);

			for (r = tp_memtable_page_first(page); r != NULL;
				 r = tp_memtable_page_next(page, r))
			{
				ItemPointerData want_ctid;
				char			want_vec[64];

				if (idx >= N)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("more records than appended (idx=%d)", idx);
				}
				ItemPointerSet(
						&want_ctid,
						(BlockNumber)(400 + idx),
						(OffsetNumber)(idx + 1));
				if (!ItemPointerEquals(&r->ctid, &want_ctid))
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"ctid mismatch at idx=%d: got (%u,%u)",
							idx,
							ItemPointerGetBlockNumber(&r->ctid),
							ItemPointerGetOffsetNumber(&r->ctid));
				}
				if (r->doc_length != 800 + idx)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"doc_length mismatch at idx=%d: got %d",
							idx,
							r->doc_length);
				}
				if (r->vector_len != vec_len)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"vector_len mismatch at idx=%d: got %u",
							idx,
							r->vector_len);
				}
				test_fill_vector(want_vec, vec_len, (uint32)(idx * 17 + 3));
				if (memcmp(r->vector_bytes, want_vec, vec_len) != 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("vector_bytes mismatch at idx=%d", idx);
				}
				idx++;
			}

			UnlockReleaseBuffer(buf);
			cur = next_blk;
		}

		if (idx != N)
			TEST_FAIL("found %d records, expected %d", idx, N);
		TEST_OK();
	}
	else if (strcmp(case_name, "oversized_rejected") == 0)
	{
		ItemPointerData ctid;
		bool			raised	= false;
		uint32			bad_len = TP_MEMTABLE_PAGE_MAX_VECTOR_LEN + 1;
		char		   *vec		= palloc0(bad_len);

		ItemPointerSet(&ctid, 1, 1);

		PG_TRY();
		{
			tp_memtable_append(rel, &ctid, 0, vec, bad_len);
		}
		PG_CATCH();
		{
			FlushErrorState();
			raised = true;
		}
		PG_END_TRY();
		pfree(vec);

		if (!raised)
			TEST_FAIL("oversized append did not raise");

		/*
		 * Chain must be untouched: a fresh index test sees
		 * an empty chain after the rejected call.
		 */
		{
			TpIndexMetaPage metap = tp_get_metapage(rel);

			if (metap->memtable_head_blkno != InvalidBlockNumber)
				TEST_FAIL(
						"chain mutated by rejected append (head=%u)",
						metap->memtable_head_blkno);
			pfree(metap);
		}
		TEST_OK();
	}

	TEST_FAIL("unknown case '%s'", case_name);
}

/*
 * Pages-info SRF.  Walks the chain from meta.head_blkno,
 * returning one row per page.
 */
PG_FUNCTION_INFO_V1(bm25_memtable_chain);

typedef struct ChainScanState
{
	Relation	rel;
	BlockNumber cur;
} ChainScanState;

Datum
bm25_memtable_chain(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	ChainScanState	*state;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext	oldcontext;
		text		   *idx_text = PG_GETARG_TEXT_PP(0);
		char		   *idx_name = text_to_cstring(idx_text);
		TupleDesc		tupdesc;
		TpIndexMetaPage metap;

		funcctx	   = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state	   = (ChainScanState *)palloc(sizeof(ChainScanState));
		state->rel = test_open_index(idx_name);
		metap	   = tp_get_metapage(state->rel);
		state->cur = metap->memtable_head_blkno;
		pfree(metap);

		tupdesc = CreateTemplateTupleDesc(4);
		TupleDescInitEntry(tupdesc, 1, "blkno", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "n_records", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "free_offset", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "next_block", INT8OID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx	= state;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state	= (ChainScanState *)funcctx->user_fctx;

	if (state->cur == InvalidBlockNumber)
	{
		index_close(state->rel, RowExclusiveLock);
		SRF_RETURN_DONE(funcctx);
	}

	{
		Buffer				  buf;
		Page				  page;
		TpMemtablePageHeader *hdr;
		Datum				  values[4];
		bool				  nulls[4] = {false, false, false, false};
		BlockNumber			  next_blk;
		HeapTuple			  tup;

		buf = ReadBuffer(state->rel, state->cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (!tp_memtable_page_is_valid(page))
		{
			UnlockReleaseBuffer(buf);
			index_close(state->rel, RowExclusiveLock);
			elog(ERROR,
				 "bm25_memtable_chain: invalid memtable page at block %u",
				 state->cur);
		}
		hdr		 = tp_memtable_page_header(page);
		next_blk = hdr->next_block;

		values[0] = Int64GetDatum((int64)state->cur);
		values[1] = Int32GetDatum((int32)hdr->n_records);
		values[2] = Int32GetDatum((int32)hdr->free_offset);
		if (next_blk == InvalidBlockNumber)
		{
			values[3] = (Datum)0;
			nulls[3]  = true;
		}
		else
		{
			values[3] = Int64GetDatum((int64)next_blk);
		}

		UnlockReleaseBuffer(buf);
		state->cur = next_blk;

		tup = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tup));
	}
}
