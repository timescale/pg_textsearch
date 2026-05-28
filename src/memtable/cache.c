/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * cache.c — apply protocol for the in-memory memtable cache.
 *
 * See cache.h for the public contract and docs/memtable_cache.md
 * for the design.  Two entry points:
 *
 *   tp_cache_cold_build()    cache.apply_lock EXCL +
 *                            cache.lock EXCL.  Allocates dshash
 *                            tables, walks the chain from
 *                            meta.head_blkno, populates the
 *                            cache, seeds the cursor.
 *
 *   tp_cache_apply_to_tail() cache.apply_lock EXCL +
 *                            cache.lock SHARED.  Generation-
 *                            checks the cursor, walks from
 *                            (cursor.next_blkno, cursor.next_off)
 *                            to the logical tail, applies each
 *                            record, advances the cursor.
 *
 * Both paths share the same record-apply primitive
 * (apply_one_record) that decodes the on-disk TpVector, runs
 * the memory-cap check, calls tp_cache_apply_document(), and
 * bumps estimated_bytes.
 *
 * The walker (src/memtable/chain_walker.c) handles per-page
 * traversal, fragment reassembly, and structural validation;
 * this module only owns the cache-side state machine.
 *
 * Scaffold SQL functions at the bottom of this file
 * (bm25_cache_apply_to_tail, bm25_cache_cold_build) provide
 * unit-level coverage; the SQL harness lives at
 * test/sql/cache_apply.sql.
 */
#include <postgres.h>

#include <access/htup_details.h>
#include <access/relation.h>
#include <catalog/index.h>
#include <fmgr.h>
#include <funcapi.h>
#include <miscadmin.h>
#include <storage/lwlock.h>
#include <utils/builtins.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "constants.h"
#include "index/metapage.h"
#include "index/resolve.h"
#include "index/state.h"
#include "memtable/cache.h"
#include "memtable/chain_walker.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "types/vector.h"

/* GUC: in mod.c, declared in kB; we work in bytes. */
extern int tp_memory_limit_kb;

/*
 * Per-index soft cap = global memory limit / 8.
 *
 * Rationale (see docs/memtable_cache.md "Memory cap"): with the
 * default 2 GiB global limit and 8 concurrent indexes, each gets
 * 256 MiB of cache headroom before catchup starts returning
 * BUDGET_EXCEEDED.  The denominator is fixed for Phase 3; the
 * 3-tier cap with eviction lands in Phase 6.
 */
#define TP_CACHE_INDEX_CAP_DIVISOR 8

uint64
tp_cache_per_index_soft_cap_bytes(void)
{
	if (tp_memory_limit_kb <= 0)
		return 0; /* sentinel: unlimited */
	return ((uint64)tp_memory_limit_kb * 1024UL) / TP_CACHE_INDEX_CAP_DIVISOR;
}

/* ---------- per-record decode + apply ---------- */

/*
 * Decoded view of one chain record's payload, ready to feed
 * into tp_cache_apply_document().
 *
 * The arrays are palloc'd in CurrentMemoryContext (typically a
 * short-lived per-record context); the caller frees them via
 * MemoryContextReset on the parent context.
 */
typedef struct DecodedDoc
{
	char **terms;		/* term_count entries, NUL-terminated */
	int32 *frequencies; /* term_count entries */
	int	   term_count;
	uint64 estimated_size; /* upper bound to charge to estimated_bytes */
} DecodedDoc;

/*
 * Decode an on-disk TpVector payload into (terms[], freqs[]).
 *
 * The on-disk byte stream IS a full v2 TpVector (varlena header,
 * magic, index name, entry stream) — see tp_memtable_append()
 * + create_tpvector_from_strings().  Fragment payloads are
 * already bounds-checked by the walker; inline payloads are
 * trusted (assert-mode validation lives in chain_source.c's
 * existing ingest_terms and is not duplicated here, since the
 * walker has tighter per-page validation).
 *
 * estimated_size is an UPPER BOUND on the bytes the apply will
 * push into the DSA arena:
 *
 *   - sizeof(TpDocLengthEntry) for the doclength insert
 *   - per term: lexeme bytes (with NUL) + sizeof(TpStringHashEntry)
 *               for the dshash key + sizeof(TpPostingEntry) for
 *               the per-doc posting slot.
 *
 * We charge the full per-term cost on every record, even though
 * shared terms reuse the existing TpStringHashEntry (the
 * dshash already-present case): this matches the design's "soft
 * cap" semantics (overshoot is acceptable; under-counting would
 * let pathological workloads bypass the cap) and avoids paying
 * for a lookup-before-charge that the apply path will repeat
 * anyway.
 */
static void
decode_record(const char *vector_bytes, uint32 vector_len, DecodedDoc *out)
{
	TpVector	  *vec;
	TpVectorEntry *entry;
	int			   n;
	uint64		   sz;

	if (vector_len == 0)
	{
		out->terms			= NULL;
		out->frequencies	= NULL;
		out->term_count		= 0;
		out->estimated_size = sizeof(TpDocLengthEntry);
		return;
	}

	vec = (TpVector *)vector_bytes;
	n	= vec->entry_count;

	out->term_count	 = n;
	out->terms		 = (n > 0) ? palloc(sizeof(char *) * n) : NULL;
	out->frequencies = (n > 0) ? palloc(sizeof(int32) * n) : NULL;

	sz = sizeof(TpDocLengthEntry);

	entry = get_tpvector_first_entry(vec);
	for (int i = 0; i < n; i++)
	{
		TpVectorEntryView v;
		char			 *term_cstr;

		entry = tpvector_entry_decode_advance(entry, &v);

		term_cstr = palloc(v.lexeme_len + 1);
		memcpy(term_cstr, v.lexeme, v.lexeme_len);
		term_cstr[v.lexeme_len] = '\0';

		out->terms[i]		= term_cstr;
		out->frequencies[i] = (int32)v.frequency;

		/*
		 * Per-term charge: lexeme bytes (+ NUL) for the interned
		 * string, plus the dshash key entry, plus the posting
		 * slot.  Underestimates the dshash bucket overhead and
		 * the posting list's growth amortisation; that's the
		 * "soft" in soft cap.
		 */
		sz += v.lexeme_len + 1;
		sz += sizeof(TpStringHashEntry);
		sz += sizeof(TpPostingEntry);
	}

	out->estimated_size = sz;
}

/*
 * Apply one decoded record to the cache.  Returns true on
 * success, false if the per-index soft cap would be exceeded
 * (caller must not advance the cursor in that case).
 *
 * Assumes the caller holds cache.apply_lock EXCL (so we are the
 * sole writer of estimated_bytes) and cache.lock at SHARED or
 * stronger (which protects the dshash table handles from being
 * reset by tp_cache_clear).
 *
 * Memory accounting is done BEFORE the apply.  If the cap check
 * passes, we apply unconditionally and add the upper-bound
 * estimate to estimated_bytes — even if the underlying
 * dshash inserts found existing entries.  The asymmetry (charge
 * the full upper bound, only credit on full drop) is intentional
 * for the soft cap.
 */
static bool
apply_one_record(
		TpLocalIndexState *local_state,
		TpMemtable		  *memtable,
		ItemPointer		   ctid,
		int32			   doc_length,
		const DecodedDoc  *doc,
		uint64			   soft_cap)
{
	uint64 cur_bytes;

	if (soft_cap > 0)
	{
		cur_bytes = pg_atomic_read_u64(&memtable->estimated_bytes);
		if (cur_bytes + doc->estimated_size > soft_cap)
			return false;
	}

	tp_cache_apply_document(
			local_state,
			ctid,
			doc->terms,
			doc->frequencies,
			doc->term_count,
			doc_length);

	pg_atomic_fetch_add_u64(&memtable->estimated_bytes, doc->estimated_size);
	return true;
}

/* ---------- walker resume position ---------- */

/*
 * Translate the cursor's "next" field into a walker open()
 * position.  The walker treats start_off=0 as "start at the
 * page's first record offset", so we don't have to special-case
 * the cold-build seed (cursor.next_off == 0, cursor.next_blkno
 * == meta.head_blkno).
 */
static inline void
cursor_to_walker_seed(
		TpMemtable *memtable, BlockNumber *out_blkno, uint16 *out_off)
{
	*out_blkno = memtable->cursor_next_blkno;
	*out_off   = memtable->cursor_next_off;
}

/* ---------- catchup ---------- */

TpCacheApplyResult
tp_cache_apply_to_tail(TpLocalIndexState *local_state, Relation rel)
{
	TpMemtable		   *memtable;
	uint64				cur_gen;
	uint64				soft_cap;
	BlockNumber			seed_blkno;
	uint16				seed_off;
	TpChainWalker	   *walker = NULL;
	TpChainWalkerRecord rec;
	MemoryContext		decode_cxt;
	MemoryContext		walker_cxt;
	MemoryContext		oldcxt;
	bool				budget_ok = true;

	Assert(local_state != NULL);
	Assert(local_state->lock_held);
	Assert(rel != NULL);

	memtable = get_memtable(local_state);
	if (memtable == NULL)
		elog(ERROR,
			 "pg_textsearch cache apply: memtable not allocated for "
			 "index oid=%u",
			 local_state->shared->index_oid);

	LWLockAcquire(&memtable->apply_lock, LW_EXCLUSIVE);
	LWLockAcquire(&memtable->lock, LW_SHARED);

	if (memtable->cursor_next_blkno == InvalidBlockNumber)
	{
		LWLockRelease(&memtable->lock);
		LWLockRelease(&memtable->apply_lock);
		return TP_CACHE_APPLY_NOT_INITIALIZED;
	}

	cur_gen = pg_atomic_read_u64(&local_state->shared->spill_generation);
	if (memtable->cursor_gen_spill_count != cur_gen)
	{
		/*
		 * Spill raced through under per-index EXCL between our
		 * caller's per-index SHARED acquire and our cache.lock
		 * acquire.  Drop the cache: re-acquire cache.lock EXCL
		 * (immediate, since spill held per-index EXCL until done,
		 * so no other reader's source can be alive across the
		 * spill gap), re-check, clear.
		 */
		LWLockRelease(&memtable->lock);
		LWLockAcquire(&memtable->lock, LW_EXCLUSIVE);
		cur_gen = pg_atomic_read_u64(&local_state->shared->spill_generation);
		if (memtable->cursor_gen_spill_count != cur_gen)
			tp_cache_clear(local_state->dsa, memtable);
		LWLockRelease(&memtable->lock);
		LWLockRelease(&memtable->apply_lock);
		return TP_CACHE_APPLY_DROPPED;
	}

	soft_cap = tp_cache_per_index_soft_cap_bytes();

	cursor_to_walker_seed(memtable, &seed_blkno, &seed_off);

	walker_cxt = AllocSetContextCreate(
			CurrentMemoryContext,
			"pg_textsearch cache apply walker",
			ALLOCSET_DEFAULT_SIZES);
	decode_cxt = AllocSetContextCreate(
			CurrentMemoryContext,
			"pg_textsearch cache apply decode",
			ALLOCSET_SMALL_SIZES);

	walker = tp_chain_walker_open(rel, seed_blkno, seed_off, walker_cxt);

	PG_TRY();
	{
		while (tp_chain_walker_next(walker, &rec))
		{
			DecodedDoc doc;

			MemoryContextReset(decode_cxt);
			oldcxt = MemoryContextSwitchTo(decode_cxt);
			decode_record(rec.vector_bytes, rec.vector_len, &doc);
			MemoryContextSwitchTo(oldcxt);

			if (!apply_one_record(
						local_state,
						memtable,
						&rec.ctid,
						rec.doc_length,
						&doc,
						soft_cap))
			{
				budget_ok = false;
				break;
			}

			memtable->cursor_next_blkno = rec.next_blkno;
			memtable->cursor_next_off	= rec.next_off;
			pg_atomic_fetch_add_u64(&memtable->cursor_seq, 1);

			CHECK_FOR_INTERRUPTS();
		}
	}
	PG_CATCH();
	{
		if (walker)
			tp_chain_walker_close(walker);
		MemoryContextDelete(decode_cxt);
		MemoryContextDelete(walker_cxt);
		LWLockRelease(&memtable->lock);
		LWLockRelease(&memtable->apply_lock);
		PG_RE_THROW();
	}
	PG_END_TRY();

	tp_chain_walker_close(walker);
	MemoryContextDelete(decode_cxt);
	MemoryContextDelete(walker_cxt);

	LWLockRelease(&memtable->lock);
	LWLockRelease(&memtable->apply_lock);

	return budget_ok ? TP_CACHE_APPLY_OK : TP_CACHE_APPLY_BUDGET_EXCEEDED;
}

/* ---------- cold build ---------- */

TpCacheColdBuildResult
tp_cache_cold_build(TpLocalIndexState *local_state, Relation rel)
{
	TpMemtable			  *memtable;
	TpIndexMetaPage		   metap;
	BlockNumber			   head;
	uint64				   gen;
	uint64				   soft_cap;
	TpChainWalker		  *walker = NULL;
	TpChainWalkerRecord	   rec;
	MemoryContext		   decode_cxt;
	MemoryContext		   walker_cxt;
	MemoryContext		   oldcxt;
	TpCacheColdBuildResult result = TP_CACHE_COLD_OK;

	Assert(local_state != NULL);
	Assert(local_state->lock_held);
	Assert(rel != NULL);

	memtable = get_memtable(local_state);
	if (memtable == NULL)
		elog(ERROR,
			 "pg_textsearch cache cold build: memtable not allocated for "
			 "index oid=%u",
			 local_state->shared->index_oid);

	LWLockAcquire(&memtable->apply_lock, LW_EXCLUSIVE);
	LWLockAcquire(&memtable->lock, LW_EXCLUSIVE);

	/*
	 * Lost-race detection: another backend cold-built (or
	 * incrementally caught up) while we were waiting on
	 * cache.lock EXCL.  RETRY so the caller goes back to
	 * apply_to_tail.
	 */
	if (memtable->cursor_next_blkno != InvalidBlockNumber)
	{
		LWLockRelease(&memtable->lock);
		LWLockRelease(&memtable->apply_lock);
		return TP_CACHE_COLD_RETRY;
	}

	/* Capture generation BEFORE walking so any spill that races
	 * after the walk completes is caught by the next catchup's
	 * generation check, not silently absorbed. */
	gen = pg_atomic_read_u64(&local_state->shared->spill_generation);

	metap = tp_get_metapage(rel);
	head  = metap->memtable_head_blkno;
	pfree(metap);

	/*
	 * Empty chain: seed the cursor to the metapage head (which
	 * may be InvalidBlockNumber) and return OK.  The next
	 * apply_to_tail will be a no-op until a writer publishes a
	 * page.
	 *
	 * Subtle: if head == InvalidBlockNumber here, we DELIBERATELY
	 * leave cursor_next_blkno at InvalidBlockNumber, which means
	 * the next apply_to_tail call returns NOT_INITIALIZED and
	 * forces another cold_build.  This is correct: an empty
	 * chain means there's nothing to cache, and a future writer
	 * that publishes the first page bumps the head — at which
	 * point the next cold_build picks up the new head_blkno.
	 * Treating "empty" as a cached state would require either
	 * watching for the head allocation (we don't) or a separate
	 * "chain head was empty" sentinel cursor state (we don't
	 * want one).
	 */
	if (head == InvalidBlockNumber)
	{
		LWLockRelease(&memtable->lock);
		LWLockRelease(&memtable->apply_lock);
		return TP_CACHE_COLD_OK;
	}

	soft_cap = tp_cache_per_index_soft_cap_bytes();

	/*
	 * Allocate the dshash tables now that we know there's work
	 * to do.  We hold cache.lock EXCL, so it is safe to mutate
	 * memtable->{string_hash_handle, doc_lengths_handle} here:
	 * no reader can be holding a SHARED lifetime lock that
	 * would observe a mid-build handle reset.  This mirrors the
	 * tp_ensure_string_table_initialized contract documented in
	 * stringtable.c (the spec requires per-index EXCL for that
	 * helper, but cache.lock EXCL is the moral equivalent for
	 * our cache-handle protection).
	 */
	tp_ensure_string_table_initialized(local_state);

	walker_cxt = AllocSetContextCreate(
			CurrentMemoryContext,
			"pg_textsearch cache cold-build walker",
			ALLOCSET_DEFAULT_SIZES);
	decode_cxt = AllocSetContextCreate(
			CurrentMemoryContext,
			"pg_textsearch cache cold-build decode",
			ALLOCSET_SMALL_SIZES);

	/*
	 * Seed the cursor.  We commit the generation token now so the
	 * apply loop's cursor advance produces a coherent (gen, blkno,
	 * off) tuple if we hit BUDGET_EXCEEDED partway through
	 * (cleared by tp_cache_clear in the abort branch below).
	 */
	memtable->cursor_gen_spill_count = gen;
	memtable->cursor_next_blkno		 = head;
	memtable->cursor_next_off		 = 0;
	pg_atomic_write_u64(&memtable->cursor_seq, 0);

	walker = tp_chain_walker_open(rel, head, 0, walker_cxt);

	PG_TRY();
	{
		while (tp_chain_walker_next(walker, &rec))
		{
			DecodedDoc doc;

			MemoryContextReset(decode_cxt);
			oldcxt = MemoryContextSwitchTo(decode_cxt);
			decode_record(rec.vector_bytes, rec.vector_len, &doc);
			MemoryContextSwitchTo(oldcxt);

			if (!apply_one_record(
						local_state,
						memtable,
						&rec.ctid,
						rec.doc_length,
						&doc,
						soft_cap))
			{
				result = TP_CACHE_COLD_ABORT;
				break;
			}

			memtable->cursor_next_blkno = rec.next_blkno;
			memtable->cursor_next_off	= rec.next_off;
			pg_atomic_fetch_add_u64(&memtable->cursor_seq, 1);

			CHECK_FOR_INTERRUPTS();
		}
	}
	PG_CATCH();
	{
		if (walker)
			tp_chain_walker_close(walker);
		MemoryContextDelete(decode_cxt);
		MemoryContextDelete(walker_cxt);
		tp_cache_clear(local_state->dsa, memtable);
		LWLockRelease(&memtable->lock);
		LWLockRelease(&memtable->apply_lock);
		PG_RE_THROW();
	}
	PG_END_TRY();

	tp_chain_walker_close(walker);
	MemoryContextDelete(decode_cxt);
	MemoryContextDelete(walker_cxt);

	if (result == TP_CACHE_COLD_ABORT)
	{
		/*
		 * Free the partially-populated cache so the caller's
		 * fallback to chain_source doesn't have stale dshash
		 * tables hanging around consuming budget.  tp_cache_clear
		 * resets the cursor to (Invalid, 0) so the next read
		 * path attempt sees NOT_INITIALIZED and re-tries
		 * cold_build (or gives up and serves from chain_source
		 * again, per the read path's two-iteration loop).
		 */
		tp_cache_clear(local_state->dsa, memtable);
	}

	LWLockRelease(&memtable->lock);
	LWLockRelease(&memtable->apply_lock);

	return result;
}

/* ---------------------------------------------------------------------
 * Scaffold SQL functions.  Internal-only unit-test entry points,
 * complementing the end-to-end coverage in the regression suite.
 *
 *   bm25_cache_cold_build(idx_name)   -> (result, records_applied,
 *                                         cursor_seq, estimated_bytes)
 *   bm25_cache_apply_to_tail(idx_name) -> (result, records_applied,
 *                                          cursor_seq, estimated_bytes)
 *
 * `records_applied` is computed as the cursor_seq delta between
 * entry and exit; it intentionally does NOT count records that
 * were rejected by the BUDGET_EXCEEDED path, because the cursor
 * is not advanced for those.
 * ---------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(bm25_cache_cold_build);
PG_FUNCTION_INFO_V1(bm25_cache_apply_to_tail);
PG_FUNCTION_INFO_V1(bm25_cache_bump_spill_generation);

static const char *
cold_build_result_text(TpCacheColdBuildResult r)
{
	switch (r)
	{
	case TP_CACHE_COLD_OK:
		return "OK";
	case TP_CACHE_COLD_RETRY:
		return "RETRY";
	case TP_CACHE_COLD_ABORT:
		return "ABORT";
	}
	return "UNKNOWN";
}

static const char *
apply_result_text(TpCacheApplyResult r)
{
	switch (r)
	{
	case TP_CACHE_APPLY_OK:
		return "OK";
	case TP_CACHE_APPLY_DROPPED:
		return "DROPPED";
	case TP_CACHE_APPLY_BUDGET_EXCEEDED:
		return "BUDGET_EXCEEDED";
	case TP_CACHE_APPLY_NOT_INITIALIZED:
		return "NOT_INITIALIZED";
	}
	return "UNKNOWN";
}

static Datum
build_result_row(
		FunctionCallInfo fcinfo,
		const char		*result_text,
		uint64			 records_applied,
		uint64			 cursor_seq,
		uint64			 estimated_bytes)
{
	TupleDesc tupdesc;
	Datum	  values[4];
	bool	  nulls[4] = {false, false, false, false};
	HeapTuple tup;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = CStringGetTextDatum(result_text);
	values[1] = Int64GetDatum((int64)records_applied);
	values[2] = Int64GetDatum((int64)cursor_seq);
	values[3] = Int64GetDatum((int64)estimated_bytes);

	tup = heap_form_tuple(tupdesc, values, nulls);
	return HeapTupleGetDatum(tup);
}

Datum
bm25_cache_cold_build(PG_FUNCTION_ARGS)
{
	text				  *idx_text = PG_GETARG_TEXT_PP(0);
	char				  *idx_name = text_to_cstring(idx_text);
	Oid					   oid		= tp_resolve_index_name_shared(idx_name);
	Relation			   rel;
	TpLocalIndexState	  *state;
	TpMemtable			  *memtable;
	uint64				   seq_before;
	uint64				   seq_after;
	uint64				   bytes_after;
	TpCacheColdBuildResult r;

	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("pg_textsearch index \"%s\" not found", idx_name)));

	rel	  = index_open(oid, AccessShareLock);
	state = tp_get_local_index_state(oid);
	if (state == NULL)
	{
		index_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("local index state not available for \"%s\"",
						idx_name)));
	}

	tp_acquire_index_lock(state, LW_SHARED);

	memtable   = get_memtable(state);
	seq_before = (memtable != NULL) ? pg_atomic_read_u64(&memtable->cursor_seq)
									: 0;

	r = tp_cache_cold_build(state, rel);

	memtable  = get_memtable(state);
	seq_after = (memtable != NULL) ? pg_atomic_read_u64(&memtable->cursor_seq)
								   : 0;
	bytes_after = (memtable != NULL)
						? pg_atomic_read_u64(&memtable->estimated_bytes)
						: 0;

	tp_release_index_lock(state);
	index_close(rel, AccessShareLock);

	return build_result_row(
			fcinfo,
			cold_build_result_text(r),
			seq_after - seq_before,
			seq_after,
			bytes_after);
}

Datum
bm25_cache_apply_to_tail(PG_FUNCTION_ARGS)
{
	text			  *idx_text = PG_GETARG_TEXT_PP(0);
	char			  *idx_name = text_to_cstring(idx_text);
	Oid				   oid		= tp_resolve_index_name_shared(idx_name);
	Relation		   rel;
	TpLocalIndexState *state;
	TpMemtable		  *memtable;
	uint64			   seq_before;
	uint64			   seq_after;
	uint64			   bytes_after;
	TpCacheApplyResult r;

	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("pg_textsearch index \"%s\" not found", idx_name)));

	rel	  = index_open(oid, AccessShareLock);
	state = tp_get_local_index_state(oid);
	if (state == NULL)
	{
		index_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("local index state not available for \"%s\"",
						idx_name)));
	}

	tp_acquire_index_lock(state, LW_SHARED);

	memtable   = get_memtable(state);
	seq_before = (memtable != NULL) ? pg_atomic_read_u64(&memtable->cursor_seq)
									: 0;

	r = tp_cache_apply_to_tail(state, rel);

	memtable  = get_memtable(state);
	seq_after = (memtable != NULL) ? pg_atomic_read_u64(&memtable->cursor_seq)
								   : 0;
	bytes_after = (memtable != NULL)
						? pg_atomic_read_u64(&memtable->estimated_bytes)
						: 0;

	tp_release_index_lock(state);
	index_close(rel, AccessShareLock);

	return build_result_row(
			fcinfo,
			apply_result_text(r),
			seq_after - seq_before,
			seq_after,
			bytes_after);
}

/*
 * bm25_cache_bump_spill_generation(index_name) -> bigint
 *
 * Permanent test scaffold for the DROPPED defensive branch of
 * tp_cache_apply_to_tail.  Real spill paths (tp_spill_finalize)
 * bump shared->spill_generation AND immediately drop the cache's
 * dshash tables via tp_cache_clear, so a subsequent apply_to_tail
 * lands on the NOT_INITIALIZED branch instead of DROPPED.  This
 * helper increments the generation without touching the tables,
 * exposing the DROPPED branch to test coverage so the safety net
 * doesn't bitrot.  The returned value is the new generation.
 *
 * INTERNAL-only; revoked from PUBLIC in the install/upgrade SQL.
 */
Datum
bm25_cache_bump_spill_generation(PG_FUNCTION_ARGS)
{
	text			  *idx_text = PG_GETARG_TEXT_PP(0);
	char			  *idx_name = text_to_cstring(idx_text);
	Oid				   oid		= tp_resolve_index_name_shared(idx_name);
	TpLocalIndexState *state;
	uint64			   new_gen;

	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("pg_textsearch index \"%s\" not found", idx_name)));

	state = tp_get_local_index_state(oid);
	if (state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("local index state not available for \"%s\"",
						idx_name)));

	new_gen = pg_atomic_add_fetch_u64(&state->shared->spill_generation, 1);
	return Int64GetDatum((int64)new_gen);
}
