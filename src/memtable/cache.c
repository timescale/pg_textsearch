/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * cache.c — lifecycle + apply protocol for the in-memory memtable cache.
 *
 * See cache.h for the public contract and docs/memtable_cache.md
 * for the design.  Three entry points:
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
 *   tp_cache_clear()         drops the dshash tables, resets the
 *                            apply cursor + estimated_bytes, and
 *                            drains accounting in lockstep with
 *                            the global counter.  Caller-supplied
 *                            lock contract (see the function
 *                            comment).
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
#include <lib/dshash.h>
#include <miscadmin.h>
#include <storage/lwlock.h>
#include <utils/builtins.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "constants.h"
#include "index/metapage.h"
#include "index/registry.h"
#include "index/resolve.h"
#include "index/state.h"
#include "memtable/cache.h"
#include "memtable/chain_walker.h"
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
 * BUDGET_EXCEEDED.  The global-cap eviction protocol layers on
 * top.
 */
#define TP_CACHE_INDEX_CAP_DIVISOR 8

/*
 * Global soft cap = memory_limit / 2; hard cap = memory_limit.
 * Both are enforced at apply-protocol entry; the per-index soft
 * cap still drives the per-record short-circuit inside apply.
 */
#define TP_CACHE_GLOBAL_SOFT_CAP_DIVISOR 2

uint64
tp_cache_per_index_soft_cap_bytes(void)
{
	if (tp_memory_limit_kb <= 0)
		return 0; /* sentinel: unlimited */
	return ((uint64)tp_memory_limit_kb * 1024UL) / TP_CACHE_INDEX_CAP_DIVISOR;
}

uint64
tp_cache_global_soft_cap_bytes(void)
{
	if (tp_memory_limit_kb <= 0)
		return 0; /* sentinel: unlimited */
	return ((uint64)tp_memory_limit_kb * 1024UL) /
		   TP_CACHE_GLOBAL_SOFT_CAP_DIVISOR;
}

uint64
tp_cache_global_hard_cap_bytes(void)
{
	if (tp_memory_limit_kb <= 0)
		return 0; /* sentinel: unlimited */
	return (uint64)tp_memory_limit_kb * 1024UL;
}

/* ---------- memory accounting ---------- */

/*
 * Add `delta` bytes to both the per-index and the global
 * estimated_bytes counters, in lockstep.  Caller must hold
 * cache.apply_lock EXCL on the target memtable; the per-index
 * counter is single-writer under that lock and the global one is
 * an atomic so concurrent updaters from other indexes compose
 * safely.
 *
 * The two atomics are deliberately NOT mutated under a single
 * lock — the global is approximate by design (the hard cap is
 * documented as approximate; see
 * docs/memtable_cache.md §"Memory cap (3 tiers)").
 */
static inline void
account_bytes_add(TpMemtable *memtable, uint64 delta)
{
	pg_atomic_fetch_add_u64(&memtable->estimated_bytes, delta);
	pg_atomic_fetch_add_u64(tp_registry_estimated_total_bytes(), delta);
}

/*
 * Drain the per-index counter to zero and subtract the drained
 * amount from the global counter.  Used by tp_cache_clear and
 * tp_cache_evict_largest.  Symmetric with account_bytes_add so
 * the global counter stays close to Σ per-index counters; a
 * raw zero-write of the per-index counter (without the global
 * subtract) would leak budget into the global counter and stall
 * future evictions.
 */
void
tp_cache_account_bytes_drain(TpMemtable *memtable)
{
	uint64 old;

	if (memtable == NULL)
		return;

	old = pg_atomic_exchange_u64(&memtable->estimated_bytes, 0);
	if (old > 0)
	{
		pg_atomic_uint64 *gp = tp_registry_estimated_total_bytes();
		uint64			  cur;
		uint64			  sub;

		/* Clamp to avoid u64 wrap on accounting drift. */
		cur = pg_atomic_read_u64(gp);
		sub = (old > cur) ? cur : old;
		if (sub > 0)
			pg_atomic_fetch_sub_u64(gp, sub);
	}
}

/* ---------- lifecycle ---------- */

void
tp_cache_clear(dsa_area *dsa, TpMemtable *memtable)
{
	if (dsa == NULL || memtable == NULL)
		return;

	if (memtable->string_hash_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_table *string_table =
				tp_string_table_attach(dsa, memtable->string_hash_handle);

		/*
		 * tp_string_table_clear walks the entries, freeing each
		 * interned term string and its posting list back to the
		 * DSA, then deletes the entry.  After that, dshash_destroy
		 * frees the dshash table's own internal allocations.
		 */
		tp_string_table_clear(dsa, string_table);
		dshash_destroy(string_table);
		memtable->string_hash_handle = DSHASH_HANDLE_INVALID;
	}

	if (memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		dshash_table *doclength_table =
				tp_doclength_table_attach(dsa, memtable->doc_lengths_handle);

		/*
		 * Doc-length entries are POD (ItemPointerData + int32) with
		 * no nested DSA allocations, so dshash_destroy alone is
		 * sufficient.
		 */
		dshash_destroy(doclength_table);
		memtable->doc_lengths_handle = DSHASH_HANDLE_INVALID;
	}

	memtable->cursor_gen_spill_count = 0;
	memtable->cursor_next_blkno		 = InvalidBlockNumber;
	memtable->cursor_next_off		 = 0;
	pg_atomic_write_u64(&memtable->cursor_seq, 0);

	/*
	 * Drain estimated_bytes and the matching slice of the global
	 * counter in lockstep so eviction accounting doesn't drift
	 * (docs/memtable_cache.md §"Memory cap (3 tiers)").
	 */
	tp_cache_account_bytes_drain(memtable);

	dsa_trim(dsa);
}

/* ---------- eviction ---------- */

/*
 * Argmax callback state.  We track (oid, dsa_pointer, bytes) of
 * the largest non-caller cache observed so far.  The dsa pointer
 * lets the post-walk path resolve the victim's shared state
 * without re-walking the registry.
 */
typedef struct EvictCandidate
{
	Oid			caller_oid;
	Oid			best_oid;
	dsa_pointer best_shared_dp;
	uint64		best_bytes;
} EvictCandidate;

/*
 * Per-entry argmax walker.  Reads memtable->estimated_bytes
 * without taking any cache lock — atomic reads are sufficient,
 * since the only mutators (apply_one_record adds,
 * tp_cache_account_bytes_drain subtracts) are themselves atomic
 * and we only need an approximate maximum for the soft-cap
 * eviction policy.
 */
static bool
evict_walk_cb(Oid oid, dsa_pointer shared_dp, void *ctx)
{
	EvictCandidate	   *c = (EvictCandidate *)ctx;
	dsa_area		   *dsa;
	TpSharedIndexState *st;
	TpMemtable		   *mt;
	uint64				bytes;

	if (oid == c->caller_oid)
		return false;
	if (!DsaPointerIsValid(shared_dp))
		return false;

	dsa = tp_registry_get_dsa();
	if (dsa == NULL)
		return false;
	st = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);
	if (st == NULL || !DsaPointerIsValid(st->memtable_dp))
		return false;

	/*
	 * Skip entries owned by a backend that is still in
	 * CREATE INDEX build mode: in that mode `st->memtable_dp` is
	 * a pointer into the building backend's PRIVATE DSA and
	 * cannot be safely dereferenced through the global DSA.  The
	 * flag is set lock-free before tp_registry_register (see
	 * src/index/state.c:tp_create_build_index_state) and cleared
	 * under tp_registry_eviction_mutex EXCL (the mutex this
	 * walker holds via tp_cache_evict_largest), so we either
	 * observe is_build_mode=true (and skip the private pointer)
	 * or is_build_mode=false (and the memtable_dp we read below
	 * is guaranteed to be a global-DSA pointer).
	 */
	if (st->is_build_mode)
		return false;

	mt = (TpMemtable *)dsa_get_address(dsa, st->memtable_dp);
	if (mt == NULL)
		return false;

	bytes = pg_atomic_read_u64(&mt->estimated_bytes);
	if (bytes > c->best_bytes)
	{
		c->best_oid		  = oid;
		c->best_shared_dp = shared_dp;
		c->best_bytes	  = bytes;
	}
	return false; /* don't stop early; scan all entries */
}

TpCacheEvictResult
tp_cache_evict_largest(Oid caller_oid)
{
	LWLock			   *mutex = tp_registry_eviction_mutex();
	dsa_area		   *dsa	  = tp_registry_get_dsa();
	EvictCandidate		c;
	TpSharedIndexState *victim;
	TpMemtable		   *vt_memtable;

	if (dsa == NULL || mutex == NULL)
		return TP_CACHE_EVICT_NOTHING_FOUND;

	LWLockAcquire(mutex, LW_EXCLUSIVE);

	c.caller_oid	 = caller_oid;
	c.best_oid		 = InvalidOid;
	c.best_shared_dp = InvalidDsaPointer;
	c.best_bytes	 = 0;

	tp_registry_walk(evict_walk_cb, &c);

	if (c.best_oid == InvalidOid || !DsaPointerIsValid(c.best_shared_dp))
	{
		LWLockRelease(mutex);
		return TP_CACHE_EVICT_NOTHING_FOUND;
	}

	victim = (TpSharedIndexState *)dsa_get_address(dsa, c.best_shared_dp);
	if (victim == NULL || !DsaPointerIsValid(victim->memtable_dp))
	{
		LWLockRelease(mutex);
		return TP_CACHE_EVICT_NOTHING_FOUND;
	}

	/*
	 * Deadlock-prevention: conditionally acquire the victim's
	 * per-index LWLock.  A reader on the victim already holds
	 * SHARED there; if that reader is also entering apply protocol
	 * entry and has just acquired the eviction_mutex queue (no,
	 * we hold it EXCL), then... actually the only concurrent
	 * holders of `victim->lock` at this moment are readers in
	 * SHARED.  Blocking on EXCL until they drain would be safe
	 * for THIS backend, but would create an
	 * eviction-vs-reader-vs-eviction cycle if a reader on the
	 * victim is itself waiting to enter evict_largest (queues
	 * for our mutex while holding victim's SHARED).  Conditional
	 * acquire turns that cycle into a benign retry.
	 */
	if (!LWLockConditionalAcquire(&victim->lock, LW_EXCLUSIVE))
	{
		LWLockRelease(mutex);
		return TP_CACHE_EVICT_BUSY;
	}

	vt_memtable = (TpMemtable *)dsa_get_address(dsa, victim->memtable_dp);
	if (vt_memtable == NULL)
	{
		LWLockRelease(&victim->lock);
		LWLockRelease(mutex);
		return TP_CACHE_EVICT_NOTHING_FOUND;
	}

	/*
	 * Re-check after acquiring victim->lock: another evict caller
	 * is impossible (we hold the mutex), but a tp_spill_finalize
	 * on the victim could have just cleared its cache via the
	 * caller's own per-index EXCL path.  Skip if drained.
	 */
	if (pg_atomic_read_u64(&vt_memtable->estimated_bytes) == 0)
	{
		LWLockRelease(&victim->lock);
		LWLockRelease(mutex);
		return TP_CACHE_EVICT_NOTHING_FOUND;
	}

	LWLockAcquire(&vt_memtable->apply_lock, LW_EXCLUSIVE);
	LWLockAcquire(&vt_memtable->lock, LW_EXCLUSIVE);
	tp_cache_clear(dsa, vt_memtable);
	LWLockRelease(&vt_memtable->lock);
	LWLockRelease(&vt_memtable->apply_lock);

	LWLockRelease(&victim->lock);
	LWLockRelease(mutex);

	return TP_CACHE_EVICT_EVICTED;
}

/*
 * Apply-protocol entry hook: check the global soft cap and
 * trigger eviction if it's exceeded.  Returns true on success
 * (cap respected, or eviction made room); false on
 * hard-cap-exceeded.
 *
 * Soft-cap eviction is a best-effort step that runs once per
 * apply-protocol entry — if it returns BUSY (another reader
 * contends for the victim's lock) we continue with the apply
 * anyway, because the hard cap below still guards against
 * uncontrolled growth.
 *
 * Called from tp_cache_apply_to_tail and tp_cache_cold_build
 * BEFORE either acquires cache.apply_lock, so the caller's own
 * cache locks are out of the chain.  See
 * docs/memtable_cache.md §"Memory cap (3 tiers)".
 */
static bool
global_cap_check(Oid caller_oid)
{
	uint64 soft = tp_cache_global_soft_cap_bytes();
	uint64 hard = tp_cache_global_hard_cap_bytes();
	uint64 cur;

	if (soft == 0 && hard == 0)
		return true; /* unlimited */

	cur = pg_atomic_read_u64(tp_registry_estimated_total_bytes());

	if (soft > 0 && cur >= soft)
	{
		(void)tp_cache_evict_largest(caller_oid);
		cur = pg_atomic_read_u64(tp_registry_estimated_total_bytes());
	}

	if (hard > 0 && cur >= hard)
		return false;
	return true;
}

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

	account_bytes_add(memtable, doc->estimated_size);
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

	/*
	 * Apply-protocol-entry hook: enforce the global cap before
	 * we take cache.apply_lock.  If eviction fails to make room
	 * and we're over the hard cap, return BUDGET_EXCEEDED so
	 * the caller falls back to chain_source.  See
	 * docs/memtable_cache.md §"Memory cap (3 tiers)".
	 */
	if (!global_cap_check(local_state->shared->index_oid))
		return TP_CACHE_APPLY_BUDGET_EXCEEDED;

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

	/*
	 * Apply-protocol-entry hook: enforce the global cap before
	 * we take cache.apply_lock.  Returning ABORT instead of
	 * building lets the caller fall back to chain_source for
	 * this query without dirtying any cache state.
	 */
	if (!global_cap_check(local_state->shared->index_oid))
		return TP_CACHE_COLD_ABORT;

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

/*
 * bm25_cache_global_estimated_bytes() -> bigint
 *
 * Returns the registry-wide estimated_total_bytes counter.
 * Permanent unit-test scaffold for the memory-cap accounting
 * protocol (docs/memtable_cache.md §"Memory cap (3 tiers)").
 * Production callers should not depend on this being precise:
 * it's a soft accounting tracker, not a resource accounting
 * source of truth.
 *
 * INTERNAL-only; revoked from PUBLIC in the install/upgrade SQL.
 */
PG_FUNCTION_INFO_V1(bm25_cache_global_estimated_bytes);

Datum
bm25_cache_global_estimated_bytes(PG_FUNCTION_ARGS)
{
	pg_atomic_uint64 *p = tp_registry_estimated_total_bytes();

	return Int64GetDatum((int64)pg_atomic_read_u64(p));
}

/*
 * bm25_cache_evict_largest(index_name) -> text
 *
 * Invokes tp_cache_evict_largest, treating `index_name` as the
 * caller's "own" index (so the argmax skips it).  Returns a
 * one-line summary string: "evicted", "nothing_found", or
 * "busy".  Used by test/sql/cache_memory_cap.sql to exercise
 * the eviction protocol without needing concurrent SQL
 * sessions or a low memory_limit GUC.
 *
 * INTERNAL-only; revoked from PUBLIC in the install/upgrade SQL.
 */
PG_FUNCTION_INFO_V1(bm25_cache_evict_largest);

Datum
bm25_cache_evict_largest(PG_FUNCTION_ARGS)
{
	text			  *idx_text = PG_GETARG_TEXT_PP(0);
	char			  *idx_name = text_to_cstring(idx_text);
	Oid				   oid		= tp_resolve_index_name_shared(idx_name);
	TpCacheEvictResult r;
	const char		  *txt;

	if (!OidIsValid(oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("pg_textsearch index \"%s\" not found", idx_name)));

	r = tp_cache_evict_largest(oid);
	switch (r)
	{
	case TP_CACHE_EVICT_EVICTED:
		txt = "evicted";
		break;
	case TP_CACHE_EVICT_BUSY:
		txt = "busy";
		break;
	case TP_CACHE_EVICT_NOTHING_FOUND:
	default:
		txt = "nothing_found";
		break;
	}
	return PointerGetDatum(cstring_to_text(txt));
}
