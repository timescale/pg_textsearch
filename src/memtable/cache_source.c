/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * cache_source.c — TpDataSource over the in-memory memtable
 *                  cache and the read-path chain/cache chooser.
 *
 * See cache_source.h for the public contract.  The constructor
 * invokes the cache apply protocol (cache.c) to bring the cache
 * up to date with the on-disk chain, then attaches the dshash
 * tables for the source lifetime and returns an ops-table-backed
 * TpDataSource that serves per-term postings, per-ctid doc
 * lengths, and per-term doc frequencies out of the cache.
 */
#include <postgres.h>

#include <fmgr.h>
#include <funcapi.h>
#include <lib/dshash.h>
#include <miscadmin.h>
#include <storage/itemptr.h>
#include <storage/lwlock.h>
#include <utils/builtins.h>
#include <utils/dsa.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "constants.h"
#include "index/resolve.h"
#include "index/source.h"
#include "index/state.h"
#include "memtable/cache.h"
#include "memtable/cache_source.h"
#include "memtable/chain_source.h"
#include "memtable/posting.h"
#include "memtable/posting_entry.h"
#include "memtable/stringtable.h"

/* GUCs read here; declared in mod.c.  tp_memtable_cache_enabled
 * gates the chooser; tp_log_cache_state emits LOG-level traces
 * at decision points for observability. */
extern bool tp_memtable_cache_enabled;
extern bool tp_log_cache_state;

/* ---------- source struct ---------- */

typedef struct TpMemtableCacheSource
{
	TpDataSource	   base;	 /* must be first */
	TpLocalIndexState *state;	 /* used for DSA + lock helpers */
	TpMemtable		  *memtable; /* cached for close() so we don't
								  * silently skip the lock release
								  * if get_memtable() ever returns
								  * NULL again. */
	dshash_table *string_table;
	dshash_table *doclength_table;

	/*
	 * If non-NULL, this is the state on which *this* source
	 * acquired the per-index LWLock SHARED; close() releases it.
	 * NULL means an outer caller already held the lock before we
	 * were constructed and is responsible for releasing it.
	 * Mirrors the chain_source convention.
	 */
	TpLocalIndexState *lock_state;

	/*
	 * True iff we hold cache.lock SHARED.  Release in close().
	 * Set false when the constructor releases on the error path
	 * before pfree.
	 */
	bool holding_cache_lock;
} TpMemtableCacheSource;

/* ---------- TpDataSourceOps implementations ---------- */

static TpPostingData *
cache_get_postings(TpDataSource *source, const char *term)
{
	TpMemtableCacheSource *cs = (TpMemtableCacheSource *)source;
	TpStringHashEntry	  *entry;
	TpPostingList		  *posting_list;
	TpPostingEntry		  *entries;
	TpPostingData		  *data;
	size_t				   term_len;
	int32				   count;
	int32				   doc_freq;
	int32				   i;

	if (term == NULL || term[0] == '\0')
		return NULL;

	term_len = strlen(term);
	entry	 = tp_string_table_lookup(
			   cs->state->dsa, cs->string_table, term, term_len);
	if (entry == NULL)
		return NULL;
	/*
	 * tp_string_table_lookup releases the dshash bucket lock
	 * before returning; the entry pointer remains valid for the
	 * lifetime of the source because we hold cache.lock SHARED
	 * (cache_clear is the only path that removes entries and it
	 * requires cache.lock EXCL).
	 */
	if (!DsaPointerIsValid(entry->key.posting_list))
		return NULL;

	posting_list = (TpPostingList *)
			dsa_get_address(cs->state->dsa, entry->key.posting_list);

	LWLockAcquire(&posting_list->lock, LW_SHARED);
	count	 = posting_list->doc_count;
	doc_freq = posting_list->doc_freq;
	if (count <= 0 || !DsaPointerIsValid(posting_list->entries_dp))
	{
		LWLockRelease(&posting_list->lock);
		return NULL;
	}
	entries = (TpPostingEntry *)
			dsa_get_address(cs->state->dsa, posting_list->entries_dp);

	data		   = tp_alloc_posting_data(count);
	data->count	   = count;
	data->doc_freq = doc_freq;
	for (i = 0; i < count; i++)
	{
		data->ctids[i]		 = entries[i].ctid;
		data->frequencies[i] = entries[i].frequency;
	}
	LWLockRelease(&posting_list->lock);

	return data;
}

static void
cache_free_postings(TpDataSource *source, TpPostingData *data)
{
	(void)source;
	tp_free_posting_data(data);
}

static int32
cache_get_doc_length(TpDataSource *source, ItemPointer ctid)
{
	TpMemtableCacheSource *cs = (TpMemtableCacheSource *)source;

	return tp_get_document_length_attached(cs->doclength_table, ctid);
}

static uint32
cache_get_doc_freq(TpDataSource *source, const char *term)
{
	TpMemtableCacheSource *cs = (TpMemtableCacheSource *)source;
	TpStringHashEntry	  *entry;
	TpPostingList		  *posting_list;
	uint32				   doc_freq;
	size_t				   term_len;

	if (term == NULL || term[0] == '\0')
		return 0;

	term_len = strlen(term);
	entry	 = tp_string_table_lookup(
			   cs->state->dsa, cs->string_table, term, term_len);
	if (entry == NULL)
		return 0;
	if (!DsaPointerIsValid(entry->key.posting_list))
		return 0;

	posting_list = (TpPostingList *)
			dsa_get_address(cs->state->dsa, entry->key.posting_list);
	LWLockAcquire(&posting_list->lock, LW_SHARED);
	doc_freq = (uint32)posting_list->doc_freq;
	LWLockRelease(&posting_list->lock);

	return doc_freq;
}

static void
cache_close(TpDataSource *source)
{
	TpMemtableCacheSource *cs = (TpMemtableCacheSource *)source;

	if (cs->string_table != NULL)
	{
		dshash_detach(cs->string_table);
		cs->string_table = NULL;
	}
	if (cs->doclength_table != NULL)
	{
		dshash_detach(cs->doclength_table);
		cs->doclength_table = NULL;
	}

	if (cs->holding_cache_lock)
	{
		Assert(cs->memtable != NULL);
		LWLockRelease(&cs->memtable->lock);
		cs->holding_cache_lock = false;
	}

	if (cs->lock_state != NULL)
	{
		tp_release_index_lock(cs->lock_state);
		cs->lock_state = NULL;
	}

	pfree(cs);
}

static const TpDataSourceOps cache_source_ops = {
		.get_postings	= cache_get_postings,
		.free_postings	= cache_free_postings,
		.get_doc_length = cache_get_doc_length,
		.get_doc_freq	= cache_get_doc_freq,
		.close			= cache_close,
};

/* ---------- helpers ---------- */

/*
 * Walk the doclength dshash once to compute corpus totals
 * (doc count + sum of lengths).  The caller holds cache.lock
 * SHARED, which keeps the table stable; the per-bucket locks
 * are taken by dshash_seq itself.
 */
static void
compute_corpus_totals(
		TpMemtableCacheSource *cs, int32 *out_total_docs, int64 *out_total_len)
{
	dshash_seq_status seq;
	TpDocLengthEntry *entry;
	uint32			  count	  = 0;
	uint64			  sum_len = 0;

	dshash_seq_init(&seq, cs->doclength_table, false);
	while ((entry = (TpDocLengthEntry *)dshash_seq_next(&seq)) != NULL)
	{
		count++;
		sum_len += (uint32)entry->doc_length;
	}
	dshash_seq_term(&seq);

	*out_total_docs = (count > (uint32)PG_INT32_MAX) ? PG_INT32_MAX
													 : (int32)count;
	*out_total_len	= (int64)(sum_len > (uint64)PG_INT64_MAX
									  ? (uint64)PG_INT64_MAX
									  : sum_len);
}

/*
 * Run the apply-protocol dance: bring the cache up to date with
 * the chain tail, cold-building once if required, with a bounded
 * number of retries on RETRY or NOT_INITIALIZED races.  Returns
 * true iff the cache is current and serveable after the call.
 *
 * Per docs/memtable_cache.md:507-536, even after `cold_build`
 * returns OK we must call `apply_to_tail` again to catch records
 * appended to the chain while the cold walk was running.  We
 * `continue` (not return) in that case.
 *
 * Caller must hold the per-index LWLock SHARED.
 */
static bool
catchup_cache(TpLocalIndexState *state, Relation rel)
{
	int retries = 0;

	while (retries++ < 4)
	{
		TpCacheApplyResult ar = tp_cache_apply_to_tail(state, rel);

		if (ar == TP_CACHE_APPLY_OK)
		{
			if (tp_log_cache_state)
				elog(LOG,
					 "pg_textsearch cache_source: catchup_ok "
					 "(oid=%u, attempt=%d)",
					 state->shared->index_oid,
					 retries);
			return true;
		}
		if (ar == TP_CACHE_APPLY_BUDGET_EXCEEDED)
		{
			if (tp_log_cache_state)
				elog(LOG,
					 "pg_textsearch cache_source: aborted_budget "
					 "(oid=%u), falling back to chain",
					 state->shared->index_oid);
			return false;
		}

		Assert(ar == TP_CACHE_APPLY_NOT_INITIALIZED ||
			   ar == TP_CACHE_APPLY_DROPPED);

		{
			TpCacheColdBuildResult cb = tp_cache_cold_build(state, rel);

			if (cb == TP_CACHE_COLD_OK)
			{
				if (tp_log_cache_state)
					elog(LOG,
						 "pg_textsearch cache_source: cold_build_ok "
						 "(oid=%u), re-applying",
						 state->shared->index_oid);
				/*
				 * Cold build returned OK but new records may have
				 * been appended to the chain while we walked.
				 * Re-enter the loop to apply them.
				 */
				continue;
			}
			if (cb == TP_CACHE_COLD_RETRY)
			{
				if (tp_log_cache_state)
					elog(LOG,
						 "pg_textsearch cache_source: cold_build_retry "
						 "(oid=%u)",
						 state->shared->index_oid);
				continue;
			}
			Assert(cb == TP_CACHE_COLD_ABORT);
			if (tp_log_cache_state)
				elog(LOG,
					 "pg_textsearch cache_source: cold_build_aborted "
					 "(oid=%u), falling back to chain",
					 state->shared->index_oid);
			return false;
		}
	}

	if (tp_log_cache_state)
		elog(LOG,
			 "pg_textsearch cache_source: catchup_retries_exhausted "
			 "(oid=%u), falling back to chain",
			 state->shared->index_oid);
	return false;
}

/* ---------- public constructors ---------- */

TpDataSource *
tp_memtable_cache_source_create(
		TpLocalIndexState *state,
		Relation		   rel,
		const char *const *query_terms,
		int				   query_term_count)
{
	TpMemtableCacheSource *cs;
	TpMemtable			  *memtable;
	TpLocalIndexState	  *lock_state_to_release;

	(void)query_terms;
	(void)query_term_count;

	Assert(state != NULL);
	Assert(rel != NULL);
	Assert(query_term_count >= 0);
	Assert(query_term_count == 0 || query_terms != NULL);

	/*
	 * Mirror chain_source's lock-ownership convention: remember
	 * whether *we* acquired the per-index lock, so close() only
	 * releases what we acquired.  If an outer caller already
	 * holds it (e.g., the scan layer), we must not release on
	 * close.
	 */
	if (state->lock_held)
		lock_state_to_release = NULL;
	else
		lock_state_to_release = state;
	tp_acquire_index_lock(state, LW_SHARED);
	if (lock_state_to_release != NULL && !state->lock_held)
		lock_state_to_release = NULL;

	memtable = get_memtable(state);
	if (memtable == NULL)
	{
		if (lock_state_to_release != NULL)
			tp_release_index_lock(lock_state_to_release);
		return NULL;
	}

	/*
	 * Run the apply protocol with per-index SHARED held.  On a
	 * non-OK terminal outcome the cache cannot serve this query;
	 * fall back to chain_source (signalled by NULL return).
	 */
	if (!catchup_cache(state, rel))
	{
		if (lock_state_to_release != NULL)
			tp_release_index_lock(lock_state_to_release);
		return NULL;
	}

	cs = (TpMemtableCacheSource *)palloc0(sizeof(TpMemtableCacheSource));
	cs->base.ops		   = &cache_source_ops;
	cs->state			   = state;
	cs->memtable		   = memtable;
	cs->lock_state		   = lock_state_to_release;
	cs->holding_cache_lock = false;
	cs->string_table	   = NULL;
	cs->doclength_table	   = NULL;

	PG_TRY();
	{
		LWLockAcquire(&memtable->lock, LW_SHARED);
		cs->holding_cache_lock = true;

		/*
		 * Re-check handle validity under cache.lock SHARED.
		 * Currently nothing concurrent can invalidate them (per-
		 * index SHARED excludes spill EXCL, and tp_cache_clear
		 * needs cache.lock EXCL which we now hold SHARED against);
		 * but the check is cheap and protects future changes that
		 * may relax that invariant (e.g. cross-index eviction).
		 * We raise an error rather than return NULL here because
		 * we already returned from catchup_cache with OK, so any
		 * invalidation is structural.
		 */
		if (memtable->string_hash_handle == DSHASH_HANDLE_INVALID ||
			memtable->doc_lengths_handle == DSHASH_HANDLE_INVALID)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("pg_textsearch cache_source: dshash handles "
							"invalidated under cache.lock SHARED "
							"(oid=%u)",
							state->shared->index_oid)));

		cs->string_table = tp_string_table_attach(
				state->dsa, memtable->string_hash_handle);
		cs->doclength_table = tp_doclength_table_attach(
				state->dsa, memtable->doc_lengths_handle);

		compute_corpus_totals(cs, &cs->base.total_docs, &cs->base.total_len);
	}
	PG_CATCH();
	{
		if (cs->string_table != NULL)
			dshash_detach(cs->string_table);
		if (cs->doclength_table != NULL)
			dshash_detach(cs->doclength_table);
		if (cs->holding_cache_lock)
			LWLockRelease(&memtable->lock);
		if (lock_state_to_release != NULL)
			tp_release_index_lock(lock_state_to_release);
		pfree(cs);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (tp_log_cache_state)
		elog(LOG,
			 "pg_textsearch cache_source: opened (oid=%u, total_docs=%d, "
			 "total_len=" INT64_FORMAT ")",
			 state->shared->index_oid,
			 cs->base.total_docs,
			 cs->base.total_len);

	return (TpDataSource *)cs;
}

TpDataSource *
tp_memtable_source_create_for_read(
		TpLocalIndexState *state,
		Relation		   rel,
		const char *const *query_terms,
		int				   query_term_count)
{
	TpDataSource *src;

	Assert(state != NULL);
	Assert(rel != NULL);

	/*
	 * Gate on the GUC and on contexts where the cache cannot be
	 * trusted to keep up with the chain.  Standbys never apply
	 * cache mutations (no shared-memory bumps from WAL replay),
	 * so they must always read the chain directly.  Build mode
	 * uses a private DSA and bypasses posting-list locks; the
	 * cache invariants do not hold there.  In all of these
	 * cases fall through to chain_source unconditionally.
	 */
	if (!tp_memtable_cache_enabled)
	{
		if (tp_log_cache_state)
			elog(LOG,
				 "pg_textsearch cache_source: disabled_by_guc, "
				 "using chain (oid=%u)",
				 state->shared->index_oid);
		return tp_memtable_chain_source_create(
				state, rel, query_terms, query_term_count);
	}
	if (RecoveryInProgress())
	{
		if (tp_log_cache_state)
			elog(LOG,
				 "pg_textsearch cache_source: disabled_by_recovery, "
				 "using chain (oid=%u)",
				 state->shared->index_oid);
		return tp_memtable_chain_source_create(
				state, rel, query_terms, query_term_count);
	}
	if (state->is_build_mode)
	{
		if (tp_log_cache_state)
			elog(LOG,
				 "pg_textsearch cache_source: disabled_by_build, "
				 "using chain (oid=%u)",
				 state->shared->index_oid);
		return tp_memtable_chain_source_create(
				state, rel, query_terms, query_term_count);
	}

	src = tp_memtable_cache_source_create(
			state, rel, query_terms, query_term_count);
	if (src != NULL)
		return src;

	if (tp_log_cache_state)
		elog(LOG,
			 "pg_textsearch cache_source: fallback_to_chain "
			 "(oid=%u)",
			 state->shared->index_oid);
	return tp_memtable_chain_source_create(
			state, rel, query_terms, query_term_count);
}

/* ---------------------------------------------------------------------
 * Scaffold SQL function for unit-level coverage.
 *
 * bm25_test_cache_source(idx_name text, case_name text) -> text
 *
 * Internal-only.  Exercises the cache_source constructor against
 * an index and reports terse outcome strings the regression test
 * compares against.
 *
 * Cases:
 *   "empty"       open a cache source against an empty memtable;
 *                 expect NULL (cache empty just like chain).
 *   "open"        open a cache source against a populated
 *                 memtable; report total_docs / total_len.
 *   "lookup:<t>"  open a cache source and look up term <t>;
 *                 report doc_freq / count, or "miss".
 *   "doclen:<n>"  open a cache source and look up the doc length
 *                 of ctid (n, 1); reports the value or -1.
 *
 * Calls tp_memtable_cache_source_create() directly so the GUC
 * gating does not affect the test; the chooser is exercised by
 * end-to-end equivalence queries in test/sql/cache_source.sql.
 */
PG_FUNCTION_INFO_V1(bm25_test_cache_source);

Datum
bm25_test_cache_source(PG_FUNCTION_ARGS)
{
	text			  *idx_name_text  = PG_GETARG_TEXT_PP(0);
	text			  *case_name_text = PG_GETARG_TEXT_PP(1);
	const char		  *idx_name		  = text_to_cstring(idx_name_text);
	const char		  *case_name	  = text_to_cstring(case_name_text);
	Oid				   idx_oid;
	Relation		   idx_rel;
	TpLocalIndexState *state;
	TpDataSource	  *src;
	StringInfoData	   buf;

	idx_oid = tp_resolve_index_name_shared(idx_name);
	if (!OidIsValid(idx_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" not found", idx_name)));

	idx_rel = index_open(idx_oid, AccessShareLock);
	state	= tp_get_local_index_state(idx_oid);
	if (state == NULL || state->shared == NULL)
	{
		index_close(idx_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("local state not initialized for index \"%s\"",
						idx_name)));
	}

	initStringInfo(&buf);

	src = tp_memtable_cache_source_create(state, idx_rel, NULL, 0);

	if (strcmp(case_name, "empty") == 0)
	{
		appendStringInfoString(
				&buf, src == NULL ? "ok:null" : "fail:non-null");
	}
	else if (strcmp(case_name, "open") == 0)
	{
		if (src == NULL)
			appendStringInfoString(&buf, "fail:null");
		else
			appendStringInfo(
					&buf,
					"ok:total_docs=%d,total_len=" INT64_FORMAT,
					src->total_docs,
					src->total_len);
	}
	else if (strncmp(case_name, "lookup:", 7) == 0)
	{
		const char *term = case_name + 7;

		if (src == NULL)
			appendStringInfoString(&buf, "fail:null");
		else
		{
			TpPostingData *data = tp_source_get_postings(src, term);
			uint32		   df	= tp_source_get_doc_freq(src, term);

			if (data == NULL)
				appendStringInfo(&buf, "miss:df=%u", df);
			else
			{
				appendStringInfo(&buf, "ok:df=%u,count=%d", df, data->count);
				tp_source_free_postings(src, data);
			}
		}
	}
	else if (strncmp(case_name, "doclen:", 7) == 0)
	{
		int				blk = atoi(case_name + 7);
		ItemPointerData ctid;

		if (src == NULL)
			appendStringInfoString(&buf, "fail:null");
		else
		{
			ItemPointerSet(&ctid, (BlockNumber)blk, 1);
			appendStringInfo(
					&buf, "ok:len=%d", tp_source_get_doc_length(src, &ctid));
		}
	}
	else
		appendStringInfo(&buf, "fail:unknown_case=%s", case_name);

	if (src != NULL)
		tp_source_close(src);

	index_close(idx_rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}
