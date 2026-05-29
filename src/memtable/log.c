/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * log.c - On-disk memtable write path.
 *
 * Memtable v2 (issue #374).  See log.h for the concurrency
 * contract and high-level semantics.
 *
 * Scaffold SQL functions are exposed at the bottom of this file:
 *
 *   bm25_test_memtable_append(index_name text, case_name text)
 *       → text         scenario harness, returns 'OK' or 'FAIL: …'
 *
 *   bm25_memtable_chain(index_name text)
 *       → SETOF (blkno bigint, n_records int, free_offset int,
 *                next_block bigint, flags int)
 *
 *   bm25_memtable_dead_pages(index_name text)
 *       → SETOF (blkno bigint, flags int, dead_fxid bigint,
 *                n_records int)
 *
 * These functions are internal-only unit-test entry points,
 * complementing the end-to-end coverage in the regression suite.
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <access/relation.h>
#include <access/transam.h>
#include <catalog/index.h>
#include <fmgr.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <storage/block.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/indexfsm.h>
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
	Buffer		buf;
	BlockNumber tail;

	buf = ReadBuffer(rel, TP_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	tail = tp_metapage_read_memtable_tail(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);

	return tail;
}

/*
 * Allocate a memtable chain page: try the index FSM first, else
 * extend the relation.  Returns a pinned buffer with
 * BUFFER_LOCK_EXCLUSIVE (same as ExtendBufferedRel EB_LOCK_FIRST).
 * Caller must UnlockReleaseBuffer when done.
 */
static Buffer
tp_memtable_alloc_page(Relation rel)
{
	BlockNumber block;

	block = GetFreeIndexPage(rel);
	if (BlockNumberIsValid(block))
	{
		if (block == TP_METAPAGE_BLKNO)
			elog(ERROR, "pg_textsearch: FSM returned metapage for memtable");

		if (block >= RelationGetNumberOfBlocks(rel))
			elog(ERROR,
				 "pg_textsearch: FSM returned block %u beyond relation "
				 "size for index \"%s\"",
				 block,
				 RelationGetRelationName(rel));

		return ReadBufferExtended(
				rel, MAIN_FORKNUM, block, RBM_ZERO_AND_LOCK, NULL);
	}

	return ExtendBufferedRel(BMR_REL(rel), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
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
		Relation			rel,
		TpSharedIndexState *shared,
		ItemPointer			ctid,
		int32				doc_length,
		const char		   *vector_bytes,
		uint32				vector_len)
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

	if (tp_metapage_read_memtable_tail(BufferGetPage(metabuf)) !=
		InvalidBlockNumber)
	{
		/* Lost the race: another backend bootstrapped first. */
		UnlockReleaseBuffer(metabuf);
		return InvalidBlockNumber;
	}

	newbuf = tp_memtable_alloc_page(rel);
	newblk = BufferGetBlockNumber(newbuf);

	xlog_state	   = GenericXLogStart(rel);
	metapage_local = GenericXLogRegisterBuffer(xlog_state, metabuf, 0);
	newpage_local  = GenericXLogRegisterBuffer(
			 xlog_state, newbuf, GENERIC_XLOG_FULL_IMAGE);

	tp_memtable_page_init(newpage_local);
	tp_memtable_page_append(
			newpage_local, ctid, doc_length, vector_bytes, vector_len);

	/*
	 * v6 -> v7 in-place upgrade (issue #383): no-op on v7 pages;
	 * on v6 pages flips version + initializes new chain fields
	 * before we overwrite them below, all within this GenericXLog.
	 */
	tp_metapage_upgrade_to_current(rel, metapage_local);

	metap = (TpIndexMetaPage)PageGetContents(metapage_local);
	metap->memtable_head_blkno = newblk;
	metap->memtable_tail_blkno = newblk;

	GenericXLogFinish(xlog_state);

	UnlockReleaseBuffer(newbuf);
	UnlockReleaseBuffer(metabuf);

	/*
	 * Bump chain page count after successful publish.  No lock
	 * needed: chain_page_count is an unsynchronized heuristic
	 * for tp_auto_spill_if_needed; the spill path (which resets
	 * it to 0) holds LW_EXCLUSIVE so concurrent inserts are
	 * fenced.
	 */
	if (shared != NULL)
		pg_atomic_fetch_add_u32(&shared->chain_page_count, 1);

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
		Relation			rel,
		TpSharedIndexState *shared,
		Buffer				tailbuf,
		ItemPointer			ctid,
		int32				doc_length,
		const char		   *vector_bytes,
		uint32				vector_len)
{
	Buffer			  newbuf;
	Buffer			  metabuf;
	BlockNumber		  newblk;
	Page			  tailpage_local;
	Page			  newpage_local;
	Page			  metapage_local;
	TpIndexMetaPage	  metap;
	GenericXLogState *xlog_state;

	newbuf = tp_memtable_alloc_page(rel);
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

	/* v6 -> v7 in-place upgrade (issue #383); see bootstrap path. */
	tp_metapage_upgrade_to_current(rel, metapage_local);

	metap = (TpIndexMetaPage)PageGetContents(metapage_local);
	metap->memtable_tail_blkno = newblk;

	GenericXLogFinish(xlog_state);

	UnlockReleaseBuffer(metabuf);
	UnlockReleaseBuffer(newbuf);

	if (shared != NULL)
		pg_atomic_fetch_add_u32(&shared->chain_page_count, 1);

	return newblk;
}

/*
 * Multi-page (fragment) write path.  Called when vector_len
 * exceeds TP_MEMTABLE_PAGE_MAX_VECTOR_LEN.
 *
 * Pre-conditions:
 *   - rel is open and the per-index LWLock is held SHARED.
 *   - old_tailbuf is either:
 *       (a) InvalidBuffer -- bootstrap path (chain was empty
 *           when we last read the metapage).  We hold no
 *           buffer lock on entry.
 *       (b) Locked EXCLUSIVE by the caller and verified valid
 *           + tail (next_block==Invalid).  We keep it locked
 *           across the sequence and release it on return.
 *
 * Layout of the new pages we create:
 *
 *   Thead (regular memtable page; fragment head record)
 *     ─► C1 (continuation page; payload chunk #1)
 *           ─► C2 ─► ... ─► Cn ─► Tnew (regular memtable page;
 *                                       initially empty, becomes
 *                                       the new tail)
 *
 * Extensions are done in reverse order (Tnew first, then
 * Cn..C1, then Thead) so each newly-initialized page already
 * knows the block number of its successor.  Each new page gets
 * its own GenericXLog record (FULL_IMAGE).
 *
 * Publish is one final GenericXLog covering at most 2 buffers:
 *   - {meta}              for bootstrap; sets head=Thead, tail=Tnew.
 *   - {old_tail, meta}    for extend; sets old_tail.next=Thead
 *                         and meta.tail=Tnew.
 *
 * Returns the block number of Thead on success, or
 * InvalidBlockNumber on a lost bootstrap race (caller should
 * retry — the already-extended pages become orphans without a
 * DEAD stamp; FSM reclaim is Step 2 in amvacuumcleanup).
 *
 * Crash safety: a crash anywhere up to and including the publish
 * GenericXLog leaves orphan pages with valid magic but
 * unreachable via meta.head → chain.  No code may sequentially
 * scan the relation without validating each page via
 * tp_memtable_page_is_valid().
 */
static BlockNumber
memtable_append_fragment(
		Relation			rel,
		TpSharedIndexState *shared,
		Buffer				old_tailbuf,
		ItemPointer			ctid,
		int32				doc_length,
		const char		   *vector_bytes,
		uint32				vector_len)
{
	uint32			  inline_cap = tp_memtable_fragment_inline_capacity_max();
	uint32			  cont_cap	 = tp_memtable_continuation_max_chunk();
	uint32			  inline_len = inline_cap;
	uint32			  remaining;
	uint32			  n_cont;
	BlockNumber		 *cont_blknos;
	BlockNumber		  tnew_blkno;
	BlockNumber		  thead_blkno;
	Buffer			  metabuf;
	GenericXLogState *xlog_state;
	bool			  bootstrap = (old_tailbuf == InvalidBuffer);

	Assert(vector_len > inline_cap);
	Assert(cont_cap > 0);

	remaining = vector_len - inline_len;
	n_cont	  = (remaining + cont_cap - 1) / cont_cap;
	Assert(n_cont >= 1);

	cont_blknos = palloc(n_cont * sizeof(BlockNumber));

	/* Step 1: extend Tnew (the post-fragment tail page). */
	{
		Buffer			  buf;
		Page			  page_local;
		GenericXLogState *x;

		buf		   = tp_memtable_alloc_page(rel);
		tnew_blkno = BufferGetBlockNumber(buf);

		x = GenericXLogStart(rel);
		page_local =
				GenericXLogRegisterBuffer(x, buf, GENERIC_XLOG_FULL_IMAGE);
		tp_memtable_page_init(page_local);
		/* next_block remains InvalidBlockNumber (tail) */

		GenericXLogFinish(x);
		UnlockReleaseBuffer(buf);
	}

	/*
	 * Step 2: extend Cn..C1 in reverse, each linking to its
	 * already-allocated successor.
	 */
	for (int i = (int)n_cont - 1; i >= 0; i--)
	{
		Buffer			  buf;
		Page			  page_local;
		GenericXLogState *x;
		BlockNumber		  next_blk;
		uint32			  chunk_off;
		uint32			  chunk_len;

		CHECK_FOR_INTERRUPTS();

		next_blk  = (i == (int)n_cont - 1) ? tnew_blkno : cont_blknos[i + 1];
		chunk_off = inline_len + (uint32)i * cont_cap;
		Assert(chunk_off < vector_len);
		chunk_len = vector_len - chunk_off;
		if (chunk_len > cont_cap)
			chunk_len = cont_cap;

		buf			   = tp_memtable_alloc_page(rel);
		cont_blknos[i] = BufferGetBlockNumber(buf);

		x = GenericXLogStart(rel);
		page_local =
				GenericXLogRegisterBuffer(x, buf, GENERIC_XLOG_FULL_IMAGE);
		tp_memtable_continuation_page_init(
				page_local, vector_bytes + chunk_off, chunk_len, next_blk);
		GenericXLogFinish(x);
		UnlockReleaseBuffer(buf);
	}

	/* Step 3: extend Thead and write the FRAGMENT head record. */
	{
		Buffer			  buf;
		Page			  page_local;
		GenericXLogState *x;

		buf			= tp_memtable_alloc_page(rel);
		thead_blkno = BufferGetBlockNumber(buf);

		x = GenericXLogStart(rel);
		page_local =
				GenericXLogRegisterBuffer(x, buf, GENERIC_XLOG_FULL_IMAGE);
		tp_memtable_page_init(page_local);
		tp_memtable_page_append_fragment_head(
				page_local,
				ctid,
				doc_length,
				vector_bytes,
				inline_len,
				vector_len);
		tp_memtable_page_set_next(page_local, cont_blknos[0]);
		GenericXLogFinish(x);
		UnlockReleaseBuffer(buf);
	}

	/* Step 4-5: publish. */
	metabuf = ReadBuffer(rel, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	if (bootstrap)
	{
		if (tp_metapage_read_memtable_tail(BufferGetPage(metabuf)) !=
			InvalidBlockNumber)
		{
			/*
			 * Lost bootstrap race: another backend populated the
			 * chain while we were extending pages.  Mark our orphaned
			 * pages DEAD so they can be reclaimed by vacuum.
			 */
			FullTransactionId horizon = ReadNextFullTransactionId();
			UnlockReleaseBuffer(metabuf);
			tp_memtable_mark_chain_dead(rel, thead_blkno, horizon);
			pfree(cont_blknos);
			return InvalidBlockNumber;
		}

		{
			Page			meta_local;
			TpIndexMetaPage metap;

			xlog_state = GenericXLogStart(rel);
			meta_local = GenericXLogRegisterBuffer(xlog_state, metabuf, 0);
			/* v6 -> v7 in-place upgrade (issue #383). */
			tp_metapage_upgrade_to_current(rel, meta_local);
			metap = (TpIndexMetaPage)PageGetContents(meta_local);
			metap->memtable_head_blkno = thead_blkno;
			metap->memtable_tail_blkno = tnew_blkno;
			GenericXLogFinish(xlog_state);
		}
		UnlockReleaseBuffer(metabuf);
	}
	else
	{
		Page			old_tail_local;
		Page			meta_local;
		TpIndexMetaPage metap;

		/*
		 * We still hold old_tailbuf EXCL.  Caller already
		 * verified old_tail.next_block == Invalid; concurrent
		 * writers are blocked by that lock until we publish.
		 */
		xlog_state	   = GenericXLogStart(rel);
		old_tail_local = GenericXLogRegisterBuffer(xlog_state, old_tailbuf, 0);
		meta_local	   = GenericXLogRegisterBuffer(xlog_state, metabuf, 0);

		tp_memtable_page_set_next(old_tail_local, thead_blkno);
		/* v6 -> v7 in-place upgrade (issue #383). */
		tp_metapage_upgrade_to_current(rel, meta_local);
		metap = (TpIndexMetaPage)PageGetContents(meta_local);
		metap->memtable_tail_blkno = tnew_blkno;

		GenericXLogFinish(xlog_state);
		UnlockReleaseBuffer(metabuf);
		UnlockReleaseBuffer(old_tailbuf);
	}

	pfree(cont_blknos);

	/*
	 * Bump chain page count by the number of pages we just
	 * published: Thead + n_cont continuation pages + Tnew.
	 * Done after both publish branches finish so we never
	 * count pages on the lost-bootstrap-race return path
	 * above.
	 */
	if (shared != NULL)
		pg_atomic_fetch_add_u32(&shared->chain_page_count, 2 + n_cont);

	return thead_blkno;
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
	 * Acquire the per-index LWLock in SHARED mode.  Spill takes
	 * it EXCLUSIVE (see build.c's tp_spill_*) so this excludes
	 * concurrent spill, while permitting other writers and
	 * readers (which both take SHARED).
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
	 * Fast / extend / bootstrap / fragment retry loop.  Each
	 * retry is caused by another backend either bootstrapping
	 * or extending the chain; under bounded concurrency the
	 * loop terminates.  We bound the iteration count
	 * defensively with INT_MAX/2 just to surface any
	 * pathological live-lock during development.
	 */
	for (int iter = 0; iter < INT_MAX / 2; iter++)
	{
		BlockNumber tail_blkno;
		Buffer		tailbuf;
		Page		tailpage;
		BlockNumber result;

		CHECK_FOR_INTERRUPTS();

		tail_blkno = memtable_read_tail_blkno(rel);

		/*
		 * Oversized record path.  Routes both the bootstrap
		 * (tail_blkno==Invalid) and extend cases through the
		 * shared fragment writer.
		 */
		if (vector_len > TP_MEMTABLE_PAGE_MAX_VECTOR_LEN)
		{
			Buffer fragment_old_tail = InvalidBuffer;

			if (tail_blkno != InvalidBlockNumber)
			{
				tailbuf = ReadBuffer(rel, tail_blkno);
				LockBuffer(tailbuf, BUFFER_LOCK_EXCLUSIVE);
				tailpage = BufferGetPage(tailbuf);

				if (!tp_memtable_page_is_valid(tailpage))
				{
					UnlockReleaseBuffer(tailbuf);
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("pg_textsearch memtable tail page at "
									"block %u in index \"%s\" is not a valid "
									"memtable page (magic mismatch)",
									tail_blkno,
									RelationGetRelationName(rel))));
				}

				if (tp_memtable_page_get_next(tailpage) != InvalidBlockNumber)
				{
					/* Stale tail.  Retry. */
					UnlockReleaseBuffer(tailbuf);
					continue;
				}

				fragment_old_tail = tailbuf;
			}

			result = memtable_append_fragment(
					rel,
					index_state->shared,
					fragment_old_tail,
					ctid,
					doc_length,
					vector_bytes,
					vector_len);
			if (result != InvalidBlockNumber)
				return result;

			/* Lost bootstrap race; retry. */
			continue;
		}

		if (tail_blkno == InvalidBlockNumber)
		{
			result = memtable_bootstrap_and_append(
					rel,
					index_state->shared,
					ctid,
					doc_length,
					vector_bytes,
					vector_len);
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
				rel,
				index_state->shared,
				tailbuf,
				ctid,
				doc_length,
				vector_bytes,
				vector_len);
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

/*
 * tp_spill_finalize -- atomic spill publication.
 *
 * Updates the metapage to point at the new segment, resets the
 * memtable chain head/tail, and bumps total_docs/total_len, all
 * inside a single GenericXLog record.  Unlinked chain pages must
 * already carry TP_MEMTABLE_PAGE_FLAG_DEAD from
 * tp_memtable_mark_chain_dead (called in tp_do_spill first).
 *
 * Caller must hold the per-index LWLock in EXCLUSIVE mode.
 *
 * If the new segment block is valid, we also splice its
 * next_segment to the previous L0 head, adding it to the same
 * GenericXLog record.  GenericXLog allows up to 4 buffers; we
 * use at most 2 (meta + segment header).
 *
 * In-memory memtable cache integration (docs/memtable_cache.md
 * §"Spill detection", §"Spill consumption from cache"):
 *
 *   1. Before touching the metapage we bump
 *      `state->shared->spill_generation` so that any reader
 *      acquiring per-index SHARED after we release EXCL observes
 *      a generation mismatch against any cursor that was captured
 *      before this spill.  The bump goes through a single atomic
 *      add; standbys never see it because spill is primary-only.
 *
 *   2. After the metapage is published we drop the cache's dshash
 *      tables via `tp_cache_clear` so the next read does a clean
 *      cold-build and we don't carry dead memory across the spill
 *      boundary.  The DROPPED defensive path in
 *      `tp_cache_apply_to_tail` remains as a belt-and-suspenders
 *      check for any cursor that captured the old generation
 *      between (1) and (2).
 *
 *   Both steps are skipped when `state` is NULL (e.g. spill paths
 *   that never wired up a local index state).
 */
#include "memtable/cache.h"
#include "segment/format.h"
#include "utils/dsa.h"

void
tp_spill_finalize(
		TpLocalIndexState *local_state,
		Relation		   rel,
		BlockNumber		   new_segment_root,
		uint64			   docs_delta,
		uint64			   len_delta)
{
	Buffer			  metabuf;
	Buffer			  seg_buf = InvalidBuffer;
	GenericXLogState *state;
	Page			  metapage;
	TpIndexMetaPage	  metap;
	BlockNumber		  old_l0_head;
	TpMemtable		 *memtable = NULL;

	Assert(rel != NULL);

	/*
	 * Step 1: bump the cache's spill-generation token BEFORE we
	 * publish the new metapage.  Cache readers that miss this
	 * happens-before edge will still detect the staleness via the
	 * cursor's captured generation on the next apply_to_tail call,
	 * but we want the common case to be "new reader sees new
	 * generation immediately and cold-builds from a clean cache".
	 */
	if (local_state != NULL && local_state->shared != NULL)
		pg_atomic_add_fetch_u64(&local_state->shared->spill_generation, 1);

	metabuf = ReadBuffer(rel, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	state	 = GenericXLogStart(rel);
	metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
	/*
	 * v6 -> v7 in-place upgrade (issue #383).  Must run before
	 * we touch any v7-only field below.  On a v6 page this
	 * piggybacks on the spill's GenericXLog record; on v7 it's
	 * a no-op.  Note: spill cannot be entered on a v6 index
	 * with unspilled chain data (v6 had no chain pages), so the
	 * helper only ever flips version + zero-inits the new
	 * fields here.
	 */
	tp_metapage_upgrade_to_current(rel, metapage);
	metap = (TpIndexMetaPage)PageGetContents(metapage);

	old_l0_head = metap->level_heads[0];

	if (new_segment_root != InvalidBlockNumber)
	{
		Page			 seg_page;
		TpSegmentHeader *seg_header;

		seg_buf = ReadBuffer(rel, new_segment_root);
		LockBuffer(seg_buf, BUFFER_LOCK_EXCLUSIVE);
		seg_page = GenericXLogRegisterBuffer(state, seg_buf, 0);
		/* Ensure pd_lower covers content for GenericXLog */
		((PageHeader)seg_page)->pd_lower = BLCKSZ;
		seg_header = (TpSegmentHeader *)PageGetContents(seg_page);
		seg_header->next_segment = old_l0_head;

		metap->level_heads[0] = new_segment_root;
		metap->level_counts[0]++;
	}

	metap->memtable_head_blkno = InvalidBlockNumber;
	metap->memtable_tail_blkno = InvalidBlockNumber;
	metap->total_docs += docs_delta;
	metap->total_len += len_delta;

	GenericXLogFinish(state);
	if (BufferIsValid(seg_buf))
		UnlockReleaseBuffer(seg_buf);
	UnlockReleaseBuffer(metabuf);

	/*
	 * Step 2: drop the in-memory cache's dshash tables.
	 *
	 * Lock order (docs/memtable_cache.md §"Lock order"):
	 *   per-index LWLock EXCL  (held by the caller, blocks all
	 *                           readers / catchup paths)
	 *     -> cache.apply_lock  (no other holder is possible, since
	 *                           every apply path takes per-index
	 *                           SHARED first; taken EXCL here
	 *                           strictly to honor the documented
	 *                           order with respect to cache.lock)
	 *       -> cache.lock      (EXCL for drop)
	 *
	 * We resolve the TpMemtable via the local index state's DSA
	 * attachment, not by reaching into the global registry, to
	 * keep this call backend-local and avoid any chance of
	 * deadlocking against the global state lock.
	 */
	if (local_state != NULL && local_state->dsa != NULL &&
		local_state->shared != NULL &&
		DsaPointerIsValid(local_state->shared->memtable_dp))
	{
		memtable = (TpMemtable *)dsa_get_address(
				local_state->dsa, local_state->shared->memtable_dp);

		if (memtable != NULL)
		{
			LWLockAcquire(&memtable->apply_lock, LW_EXCLUSIVE);
			LWLockAcquire(&memtable->lock, LW_EXCLUSIVE);
			tp_cache_clear(local_state->dsa, memtable);
			LWLockRelease(&memtable->lock);
			LWLockRelease(&memtable->apply_lock);
		}
	}
}

/*
 * WAL-log DEAD + dead_fxid on one memtable page (delta record).
 * Caller must hold an exclusive lock on buf.
 */
static void
tp_memtable_wal_mark_page_dead_buf(
		Relation rel, Buffer buf, FullTransactionId horizon)
{
	Page			  page;
	GenericXLogState *state;

	Assert(BufferIsValid(buf));

	state = GenericXLogStart(rel);
	page  = GenericXLogRegisterBuffer(state, buf, 0);

	if (!tp_memtable_page_is_dead(page))
		tp_memtable_page_mark_dead(page, horizon);

	GenericXLogFinish(state);
}

/*
 * Mark continuation pages for one FRAGMENT record.  Returns the
 * first block that is not a continuation page (resume outer walk).
 */
static BlockNumber
tp_memtable_mark_continuation_chain_dead(
		Relation rel, BlockNumber blk, FullTransactionId horizon)
{
	BlockNumber cur = blk;

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		TpMemtablePageHeader *hdr;
		BlockNumber			  next;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(rel, cur);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		page = BufferGetPage(buf);
		hdr	 = tp_memtable_page_header(page);

		if (!tp_memtable_page_is_valid(page))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch memtable marking dead chain page "
							"inner walk at block %u in index \"%s\" has "
							"invalid magic",
							cur,
							RelationGetRelationName(rel))));
		}
		if (!tp_memtable_page_is_continuation(page))
		{
			UnlockReleaseBuffer(buf);
			return cur;
		}

		next = hdr->next_block;
		tp_memtable_wal_mark_page_dead_buf(rel, buf, horizon);
		UnlockReleaseBuffer(buf);
		cur = next;
	}
	return cur;
}

/*
 * Mark every page in the spilled memtable chain DEAD (outer walk +
 * fragment continuation sub-chains).  See docs/memtable_v2.md spill
 * step 4.  Does not free blocks; amvacuumcleanup recycles later.
 */
void
tp_memtable_mark_chain_dead(
		Relation rel, BlockNumber head, FullTransactionId horizon)
{
	BlockNumber cur;

	if (head == InvalidBlockNumber)
		return;

	cur = head;

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		TpMemtablePageHeader *hdr;
		TpMemtableRecord	 *rec;
		BlockNumber			  next;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(rel, cur);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);

		if (!tp_memtable_page_is_valid(page))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch memtable marking dead chain page "
							"outer walk at block %u in index \"%s\" has "
							"invalid magic",
							cur,
							RelationGetRelationName(rel))));
		}
		if (tp_memtable_page_is_continuation(page))
		{
			UnlockReleaseBuffer(buf);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pg_textsearch memtable marking dead page outer "
							"walk reached invalid continuation page at "
							"block %u in index \"%s\"",
							cur,
							RelationGetRelationName(rel))));
		}

		hdr	 = tp_memtable_page_header(page);
		next = hdr->next_block;
		rec	 = tp_memtable_page_first(page);

		if (rec != NULL &&
			(rec->flags & TP_MEMTABLE_RECORD_FLAG_FRAGMENT) != 0)
		{
			/*
			 * Inner walk marks C1..Cn; returns the first
			 * non-continuation block (usually the post-fragment
			 * tail), same resume rule as chain_source.c.
			 */
			next = tp_memtable_mark_continuation_chain_dead(
					rel, next, horizon);
		}

		tp_memtable_wal_mark_page_dead_buf(rel, buf, horizon);
		UnlockReleaseBuffer(buf);

		cur = next;
	}
}

/*
 * Append a single document to the on-disk memtable chain and
 * bump the per-index counters used by auto-spill heuristics.
 *
 * Memtable v2 (issue #374): the previous DSA write path
 * (dshash inserts + tp_store_document_length +
 * tp_add_document_to_posting_list + idempotency gate) is
 * replaced by an unconditional tp_memtable_append() into the
 * on-disk chain.  Crash idempotency is provided by GenericXLog
 * LSN-checked redo at the page level, not by application-level
 * deduplication.
 *
 * The `vector_bytes` payload is an in-memory v2 TpVector; only
 * its byte image is appended to the chain.  `term_count` is
 * used to bump the bulk-load detection counter; the chain
 * source reconstructs per-term postings on read.
 */
void
tp_add_document_terms(
		TpLocalIndexState *local_state,
		Relation		   rel,
		ItemPointer		   ctid,
		const char		  *vector_bytes,
		uint32			   vector_len,
		int				   term_count,
		int32			   doc_length)
{
	tp_memtable_append(rel, ctid, doc_length, vector_bytes, vector_len);

	/* Track terms added in this transaction for bulk load detection. */
	local_state->terms_added_this_xact += term_count;
}

/* ---------------------------------------------------------------------
 * Scaffold SQL functions.  Internal-only unit-test entry points,
 * complementing the end-to-end coverage in the regression suite.
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
	else if (strcmp(case_name, "multi_page_fragment") == 0)
	{
		/*
		 * Append three records in sequence:
		 *   1. A small record on a fresh page (ctid1).
		 *   2. A record whose vector_len exceeds
		 *      TP_MEMTABLE_PAGE_MAX_VECTOR_LEN (ctid2).  This
		 *      forces the fragment branch: head page + N
		 *      continuation pages + a fresh tail page.
		 *   3. Another small record (ctid3), which should
		 *      land on the just-extended fresh tail.
		 *
		 * Verify the resulting chain shape page-by-page and
		 * that the fragment head record's metadata
		 * round-trips (full vector_len, FRAGMENT flag, inline
		 * prefix bytes match the input).
		 */
		ItemPointerData ctid1, ctid2, ctid3;
		uint32			small_len = 64;
		uint32			large_len;
		char		   *small_vec;
		char		   *large_vec;
		uint32			inline_cap;
		uint32			cont_cap;
		uint32			expected_n_cont;
		uint32			expected_total_pages;
		uint32			observed_pages = 0;
		BlockNumber		head_blk;
		BlockNumber		tail_blk;
		BlockNumber		cur;
		TpIndexMetaPage metap;

		inline_cap		= tp_memtable_fragment_inline_capacity_max();
		cont_cap		= tp_memtable_continuation_max_chunk();
		large_len		= inline_cap + (uint32)(2.5 * (double)cont_cap);
		expected_n_cont = (large_len - inline_cap + cont_cap - 1) / cont_cap;
		/* small_page + Thead + Cn + Tnew (the post-fragment tail). */
		expected_total_pages = 1 + 1 + expected_n_cont + 1;

		small_vec = palloc(small_len);
		large_vec = palloc(large_len);
		test_fill_vector(small_vec, small_len, 11);
		test_fill_vector(large_vec, large_len, 22);

		ItemPointerSet(&ctid1, 1, 1);
		tp_memtable_append(rel, &ctid1, 10, small_vec, small_len);

		ItemPointerSet(&ctid2, 2, 1);
		tp_memtable_append(rel, &ctid2, 20, large_vec, large_len);

		ItemPointerSet(&ctid3, 3, 1);
		tp_memtable_append(rel, &ctid3, 30, small_vec, small_len);

		metap	 = tp_get_metapage(rel);
		head_blk = metap->memtable_head_blkno;
		tail_blk = metap->memtable_tail_blkno;
		pfree(metap);

		if (head_blk == InvalidBlockNumber)
			TEST_FAIL("head_blk Invalid after appends");
		if (tail_blk == InvalidBlockNumber)
			TEST_FAIL("tail_blk Invalid after appends");

		cur = head_blk;
		while (cur != InvalidBlockNumber)
		{
			Buffer				  buf;
			Page				  page;
			TpMemtablePageHeader *hdr;
			BlockNumber			  next_blk;

			buf = ReadBuffer(rel, cur);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			page = BufferGetPage(buf);
			if (!tp_memtable_page_is_valid(page))
			{
				UnlockReleaseBuffer(buf);
				TEST_FAIL("invalid page at block %u", cur);
			}
			hdr		 = tp_memtable_page_header(page);
			next_blk = hdr->next_block;

			if (observed_pages == 0)
			{
				/* Original small_record page (ctid1). */
				TpMemtableRecord *r;

				if ((hdr->flags & TP_MEMTABLE_PAGE_FLAG_CONTINUATION) != 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("first page has CONTINUATION flag");
				}
				if (hdr->n_records != 1)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"first page n_records=%u, expected 1",
							hdr->n_records);
				}
				r = tp_memtable_page_first(page);
				if ((r->flags & TP_MEMTABLE_RECORD_FLAG_FRAGMENT) != 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("first page record has FRAGMENT flag");
				}
				if (!ItemPointerEquals(&r->ctid, &ctid1))
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("first page record ctid mismatch");
				}
			}
			else if (observed_pages == 1)
			{
				/* Fragment head page. */
				TpMemtableRecord *r;

				if ((hdr->flags & TP_MEMTABLE_PAGE_FLAG_CONTINUATION) != 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("fragment head has CONTINUATION flag");
				}
				if (hdr->n_records != 1)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"fragment head n_records=%u, expected 1",
							hdr->n_records);
				}
				r = tp_memtable_page_first(page);
				if ((r->flags & TP_MEMTABLE_RECORD_FLAG_FRAGMENT) == 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("fragment head record missing FRAGMENT flag");
				}
				if (r->vector_len != large_len)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"fragment vector_len=%u, expected %u",
							r->vector_len,
							large_len);
				}
				if (!ItemPointerEquals(&r->ctid, &ctid2))
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("fragment head ctid mismatch");
				}
				/* Spot-check inline prefix bytes. */
				if (memcmp(r->vector_bytes, large_vec, 32) != 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("fragment inline prefix bytes mismatch");
				}
			}
			else if (observed_pages < 2 + expected_n_cont)
			{
				/* Continuation page. */
				if ((hdr->flags & TP_MEMTABLE_PAGE_FLAG_CONTINUATION) == 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"expected continuation flag at page %u",
							observed_pages);
				}
				if (hdr->n_records != 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"continuation n_records=%u, expected 0",
							hdr->n_records);
				}
			}
			else
			{
				/* Tnew tail page containing ctid3. */
				TpMemtableRecord *r;

				if (cur != tail_blk)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"last page is block %u, but meta.tail=%u",
							cur,
							tail_blk);
				}
				if ((hdr->flags & TP_MEMTABLE_PAGE_FLAG_CONTINUATION) != 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("tail page has CONTINUATION flag");
				}
				if (hdr->n_records != 1)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL(
							"tail page n_records=%u, expected 1",
							hdr->n_records);
				}
				r = tp_memtable_page_first(page);
				if ((r->flags & TP_MEMTABLE_RECORD_FLAG_FRAGMENT) != 0)
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("tail page record has FRAGMENT flag");
				}
				if (!ItemPointerEquals(&r->ctid, &ctid3))
				{
					UnlockReleaseBuffer(buf);
					TEST_FAIL("tail page ctid mismatch");
				}
			}

			UnlockReleaseBuffer(buf);
			observed_pages++;
			cur = next_blk;
		}

		if (observed_pages != expected_total_pages)
			TEST_FAIL(
					"observed %u pages, expected %u (n_cont=%u)",
					observed_pages,
					expected_total_pages,
					expected_n_cont);

		pfree(small_vec);
		pfree(large_vec);
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

		tupdesc = CreateTemplateTupleDesc(5);
		TupleDescInitEntry(tupdesc, 1, "blkno", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "n_records", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "free_offset", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "next_block", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 5, "flags", INT4OID, -1, 0);
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
		Datum				  values[5];
		bool				  nulls[5] = {false, false, false, false, false};
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
		values[4] = Int32GetDatum((int32)hdr->flags);

		UnlockReleaseBuffer(buf);
		state->cur = next_blk;

		tup = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tup));
	}
}

/*
 * Dead-orphan SRF.  Scans index blocks after the metapage and
 * returns one row per memtable page stamped DEAD by spill (step 4).
 */
PG_FUNCTION_INFO_V1(bm25_memtable_dead_pages);

typedef struct DeadScanState
{
	Relation	rel;
	BlockNumber cur;
	BlockNumber nblocks;
} DeadScanState;

Datum
bm25_memtable_dead_pages(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	DeadScanState	*state;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		text		 *idx_text = PG_GETARG_TEXT_PP(0);
		char		 *idx_name = text_to_cstring(idx_text);
		TupleDesc	  tupdesc;

		funcctx	   = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state		   = (DeadScanState *)palloc(sizeof(DeadScanState));
		state->rel	   = test_open_index(idx_name);
		state->nblocks = RelationGetNumberOfBlocks(state->rel);
		state->cur	   = TP_METAPAGE_BLKNO + 1;

		tupdesc = CreateTemplateTupleDesc(4);
		TupleDescInitEntry(tupdesc, 1, "blkno", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "flags", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "dead_fxid", INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "n_records", INT4OID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx	= state;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state	= (DeadScanState *)funcctx->user_fctx;

	while (state->cur < state->nblocks)
	{
		Buffer				  buf;
		Page				  page;
		TpMemtablePageHeader *hdr;
		BlockNumber			  blk = state->cur;
		Datum				  values[4];
		bool				  nulls[4] = {false, false, false, false};
		HeapTuple			  tup;

		state->cur++;

		buf = ReadBuffer(state->rel, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		if (!tp_memtable_page_is_valid(page) ||
			!tp_memtable_page_is_dead(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		hdr = tp_memtable_page_header(page);

		values[0] = Int64GetDatum((int64)blk);
		values[1] = Int32GetDatum((int32)hdr->flags);
		values[2] = Int64GetDatum(
				(int64)U64FromFullTransactionId(hdr->dead_fxid));
		values[3] = Int32GetDatum((int32)hdr->n_records);

		UnlockReleaseBuffer(buf);

		tup = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tup));
	}

	index_close(state->rel, RowExclusiveLock);
	SRF_RETURN_DONE(funcctx);
}
