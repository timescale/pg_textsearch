/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * scan.c - BM25 index scan operations
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/relscan.h>
#include <access/sdir.h>
#include <access/table.h>
#include <catalog/namespace.h>
#include <miscadmin.h>
#include <pgstat.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/hsearch.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/regproc.h>
#include <utils/rel.h>

#include "access/am.h"
#include "access/boolean.h"
#include "constants.h"
#include "index/limit.h"
#include "index/metapage.h"
#include "index/resolve.h"
#include "index/state.h"
#include "memtable/scan.h"
#include "types/query.h"
#include "types/vector.h"

/*
 * Backend-local cached score for ORDER BY optimization.
 *
 * When tp_gettuple returns a row, the BM25 score is cached here. The
 * bm25_get_current_score() stub function returns this value, avoiding
 * re-computation of scores in resjunk ORDER BY expressions.
 */
static float8 tp_cached_score = 0.0;

static bool
tp_is_combined_scan(IndexScanDesc scan, TpScanOpaque so)
{
	return so->is_boolean_scan && scan->numberOfOrderBys > 0;
}

float8
tp_get_cached_score(void)
{
	return tp_cached_score;
}

/* Track CTIDs already emitted by this scan. */
static bool
tp_ctid_seen_or_mark(TpScanOpaque so, ItemPointer tid)
{
	bool found;

	if (so->returned_ctids == NULL)
	{
		HASHCTL ctl;
		long	nelem;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize	  = sizeof(ItemPointerData);
		ctl.entrysize = sizeof(ItemPointerData);
		ctl.hcxt	  = so->scan_context;

		/* Use the current batch size as the initial dynahash hint. */
		nelem = so->max_results_used > 0 ? so->max_results_used : 256;

		so->returned_ctids = hash_create(
				"Tapir scan returned ctids",
				nelem,
				&ctl,
				HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	(void)hash_search(so->returned_ctids, tid, HASH_ENTER, &found);
	return found;
}

/* Reset emitted-CTID tracking for a restarted scan. */
static void
tp_returned_ctids_reset(TpScanOpaque so)
{
	if (so && so->returned_ctids)
	{
		hash_destroy(so->returned_ctids);
		so->returned_ctids = NULL;
	}
}

/*
 * Clean up any previous scan results in the scan opaque structure
 */
static void
tp_rescan_cleanup_results(TpScanOpaque so)
{
	if (!so)
		return;

	Assert(so->scan_context != NULL);

	/* Clean up result CTIDs */
	if (so->result_ctids)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);
		pfree(so->result_ctids);
		so->result_ctids = NULL;
		MemoryContextSwitchTo(oldcontext);
	}

	/* Clean up result scores */
	if (so->result_scores)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);
		pfree(so->result_scores);
		so->result_scores = NULL;
		MemoryContextSwitchTo(oldcontext);
	}

	if (so->boolean_results)
	{
		BufFileClose(so->boolean_results);
		so->boolean_results = NULL;
	}

	if (so->boolean_matches)
	{
		BufFileClose(so->boolean_matches);
		so->boolean_matches = NULL;
	}

	if (so->boolean_matched_ctids)
	{
		hash_destroy(so->boolean_matched_ctids);
		so->boolean_matched_ctids = NULL;
	}
}

/*
 * Process ORDER BY scan keys for <@> operator
 *
 * Handles both bm25query and plain text arguments to support:
 * - ORDER BY content <@> 'query'::bm25query (explicit bm25query)
 * - ORDER BY content <@> 'query' (plain text, implicit index resolution)
 */
static void
tp_rescan_process_orderby(
		IndexScanDesc	scan,
		ScanKey			orderbys,
		int				norderbys,
		TpIndexMetaPage metap)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;

	for (int i = 0; i < norderbys; i++)
	{
		ScanKey orderby = &orderbys[i];

		/* Check for <@> operator strategy */
		if (orderby->sk_strategy == 1) /* Strategy 1: <@> operator */
		{
			Datum query_datum = orderby->sk_argument;
			char *query_cstr;
			Oid	  query_index_oid = InvalidOid;

			/*
			 * Use sk_subtype to determine the argument type.
			 * sk_subtype contains the right-hand operand's type OID.
			 */
			if (orderby->sk_subtype == TEXTOID)
			{
				/* Plain text - use text directly */
				text *query_text = (text *)DatumGetPointer(query_datum);

				query_cstr = text_to_cstring(query_text);
			}
			else
			{
				/* bm25query - extract query text and index OID */
				TpQuery *query = (TpQuery *)DatumGetPointer(query_datum);

				query_cstr		= pstrdup(get_tpquery_text(query));
				query_index_oid = get_tpquery_index_oid(query);

				/* Validate index OID if provided in query */
				if (tpquery_has_index(query))
				{
					tp_validate_query_index(
							query_index_oid, scan->indexRelation);
				}
			}

			/* Clear query vector since we're using text directly */
			if (so->query_vector)
			{
				pfree(so->query_vector);
				so->query_vector = NULL;
			}

			/* Free old query text if it exists */
			if (so->query_text)
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(
						so->scan_context);
				pfree(so->query_text);
				MemoryContextSwitchTo(oldcontext);
			}

			/* Allocate new query text in scan context */
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(
						so->scan_context);
				so->query_text = pstrdup(query_cstr);
				MemoryContextSwitchTo(oldcontext);
			}

			/* Store index OID for this scan */
			so->index_oid = RelationGetRelid(scan->indexRelation);

			/* Mark all docs as candidates for ORDER BY operation */
			if (metap && metap->total_docs > 0)
				so->result_count = metap->total_docs;

			pfree(query_cstr);
		}
	}
}

/*
 * Begin a scan of the Tapir index
 */
IndexScanDesc
tp_beginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	TpScanOpaque  so;

	/*
	 * Surface the durable "results may be incomplete" marker left by
	 * an in-place pre-v1.3 upgrade so read-only workloads
	 * see it; throttled to once per index per session.
	 */
	tp_warn_if_pending_docid(index);

	scan = RelationGetIndexScan(index, nkeys, norderbys);

	/* Allocate and initialize scan opaque data */
	so				 = (TpScanOpaque)palloc0(sizeof(TpScanOpaqueData));
	so->scan_context = AllocSetContextCreate(
			CurrentMemoryContext,
			"Tapir Scan Context",
			ALLOCSET_DEFAULT_SIZES);
	so->boolean_context = AllocSetContextCreate(
			so->scan_context,
			"Tapir Boolean Scan Context",
			ALLOCSET_DEFAULT_SIZES);
	so->limit			 = -1; /* Initialize limit to -1 (no limit) */
	so->max_results_used = 0;
	scan->opaque		 = so;

	/*
	 * Custom index AMs must allocate ORDER BY arrays themselves.
	 */
	if (norderbys > 0)
	{
		scan->xs_orderbyvals  = (Datum *)palloc0(sizeof(Datum) * norderbys);
		scan->xs_orderbynulls = (bool *)palloc(sizeof(bool) * norderbys);
		/* Initialize all orderbynulls to true */
		memset(scan->xs_orderbynulls, true, sizeof(bool) * norderbys);
	}

	return scan;
}

/*
 * Restart a scan with new keys
 */
void
tp_rescan(
		IndexScanDesc scan,
		ScanKey		  keys,
		int			  nkeys,
		ScanKey		  orderbys,
		int			  norderbys)
{
	TpScanOpaque	so	  = (TpScanOpaque)scan->opaque;
	TpIndexMetaPage metap = NULL;

	Assert(scan != NULL);
	Assert(scan->opaque != NULL);

	if (!so)
		return;

	/* Retrieve query LIMIT, if available */
	{
		int query_limit = tp_get_query_limit(scan->indexRelation);
		so->limit		= (query_limit > 0) ? query_limit : -1;
	}

	/* Reset scan state */
	if (so)
	{
		/* Clean up any previous results */
		tp_rescan_cleanup_results(so);

		/* Release scratch allocations from the previous Boolean execution. */
		MemoryContextReset(so->boolean_context);

		/* Drop the emitted-CTID dedup set from any prior scan */
		tp_returned_ctids_reset(so);

		/* Reset scan position and state */
		so->current_pos	 = 0;
		so->result_count = 0;
		so->eof_reached	 = false;
		so->query_vector = NULL;

		/*
		 * NULL keys restart the scan with its previous keys.  Only discard
		 * the copied Boolean query when PostgreSQL supplies replacements.
		 */
		if (keys != NULL)
		{
			if (so->boolean_query != NULL)
			{
				pfree(so->boolean_query);
				so->boolean_query = NULL;
			}
			so->is_boolean_scan = false;
			so->boolean_recheck = false;
		}
	}

	if (nkeys > 0 && keys && so)
	{
		if (!metap)
			metap = tp_get_metapage(scan->indexRelation);

		tp_boolean_rescan(scan, keys, nkeys, metap);
	}

	/* Process ORDER BY scan keys for <@> operator */
	if (norderbys > 0 && orderbys && so)
	{
		/* Get index metadata to check if we have documents */
		if (!metap)
			metap = tp_get_metapage(scan->indexRelation);

		tp_rescan_process_orderby(scan, orderbys, norderbys, metap);
	}

	if (tp_is_combined_scan(scan, so))
		so->boolean_recheck = true;

	if (metap)
		pfree(metap);
}

/*
 * End a scan and cleanup resources
 */
void
tp_endscan(IndexScanDesc scan)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;

	if (so)
	{
		tp_rescan_cleanup_results(so);

		if (so->scan_context)
			MemoryContextDelete(so->scan_context);

		/* Free query vector if it was allocated */
		if (so->query_vector)
			pfree(so->query_vector);

		pfree(so);
		scan->opaque = NULL;
	}

	/*
	 * Don't free ORDER BY arrays here - PostgreSQL's core code will free them.
	 */
	if (scan->numberOfOrderBys > 0)
	{
		scan->xs_orderbyvals  = NULL;
		scan->xs_orderbynulls = NULL;
	}
}

/*
 * Execute BM25 scoring query to get ordered results
 */
static bool
tp_execute_scoring_query(IndexScanDesc scan)
{
	TpScanOpaque	   so = (TpScanOpaque)scan->opaque;
	TpIndexMetaPage	   metap;
	bool			   success	   = false;
	TpLocalIndexState *index_state = NULL;
	TpVector		  *query_vector;

	if (!so || !so->query_text)
		return false;

	Assert(so->scan_context != NULL);

	/* Clean up previous results */
	if (so->result_ctids || so->result_scores)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);

		if (so->result_ctids)
		{
			pfree(so->result_ctids);
			so->result_ctids = NULL;
		}
		if (so->result_scores)
		{
			pfree(so->result_scores);
			so->result_scores = NULL;
		}

		MemoryContextSwitchTo(oldcontext);
	}

	so->result_count = 0;
	so->current_pos	 = 0;

	/* Get the index state with posting lists */
	index_state = tp_get_local_index_state(
			RelationGetRelid(scan->indexRelation));

	if (!index_state)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for BM25 "
						"search")));
	}

	/*
	 * Acquire shared lock BEFORE reading metapage.
	 * This ensures the metapage and memtable are read in a
	 * consistent state — spill (which rewrites both) requires
	 * LW_EXCLUSIVE, which is blocked while we hold shared.
	 */
	tp_acquire_index_lock(index_state, LW_SHARED);

	/* Now read metapage under the lock */
	metap = tp_get_metapage(scan->indexRelation);
	if (!metap)
	{
		tp_release_index_lock(index_state);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to get metapage for index %s",
						RelationGetRelationName(scan->indexRelation))));
	}

	/* Use the original query vector or create one from text */
	query_vector = so->query_vector;

	if (!query_vector && so->query_text)
	{
		/*
		 * We have a text query - convert it to a vector using the index.
		 */
		char *index_name = tp_get_qualified_index_name(scan->indexRelation);

		text *index_name_text  = cstring_to_text(index_name);
		text *query_text_datum = cstring_to_text(so->query_text);

		Datum query_vec_datum = DirectFunctionCall2(
				to_tpvector,
				PointerGetDatum(query_text_datum),
				PointerGetDatum(index_name_text));

		query_vector = (TpVector *)DatumGetPointer(query_vec_datum);

		/* Free existing query vector if present */
		if (so->query_vector)
			pfree(so->query_vector);

		/* Store the converted vector for this query execution */
		so->query_vector = query_vector;
	}

	if (!query_vector)
	{
		pfree(metap);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("no query vector available in scan state")));
	}

	/* Find documents matching the query using posting lists */
	success = tp_memtable_search(scan, index_state, query_vector, metap);

	/* Release the lock - we've extracted all CTIDs we need */
	tp_release_index_lock(index_state);

	pfree(metap);
	return success;
}

/*
 * Bound the combined-scan lookup set by work_mem.  dynahash needs roughly
 * this many bytes for one CTID entry plus its share of the bucket directory.
 */
#define TP_BOOLEAN_FILTER_ENTRY_BYTES 32

/*
 * Load the materialized Boolean matches into a CTID lookup set.
 *
 * A match set larger than work_mem keeps the streaming result file only, so
 * ranked candidates fall back to the heap recheck instead of paying for an
 * unbounded hash table.
 */
static void
tp_boolean_filter_build(IndexScanDesc scan)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;
	HASHCTL		 ctl;
	long		 max_entries;

	Assert(so->boolean_results != NULL);

	max_entries = (long)work_mem * 1024L / TP_BOOLEAN_FILTER_ENTRY_BYTES;
	if (so->result_count > max_entries)
		return;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize	  = sizeof(ItemPointerData);
	ctl.entrysize = sizeof(ItemPointerData);
	ctl.hcxt	  = so->scan_context;

	so->boolean_matched_ctids = hash_create(
			"Tapir Boolean matched ctids",
			so->result_count,
			&ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	while (tp_boolean_next(scan))
		(void)hash_search(
				so->boolean_matched_ctids,
				&scan->xs_heaptid,
				HASH_ENTER,
				NULL);
}

/*
 * Evaluate the Boolean query once for a combined scan.
 *
 * The Boolean executor already materializes every matching CTID, so a single
 * evaluation both rejects ranked candidates inside the index and later
 * supplies the zero-score Boolean tail.  Returns false when the predicate
 * matches nothing, which ends the scan.
 */
static bool
tp_boolean_filter_prepare(IndexScanDesc scan)
{
	TpScanOpaque	   so = (TpScanOpaque)scan->opaque;
	TpLocalIndexState *index_state;
	int				   saved_count = so->result_count;
	int				   saved_pos   = so->current_pos;
	bool			   matched;

	index_state = tp_get_local_index_state(
			RelationGetRelid(scan->indexRelation));
	if (!index_state)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for BM25 Boolean search")));

	matched = tp_boolean_execute(scan, index_state);
	if (matched)
		tp_boolean_filter_build(scan);

	/* Ranked candidates that were never checked still need the recheck. */
	if (so->boolean_matched_ctids == NULL)
		so->boolean_recheck = true;

	/* Keep the matches for the tail and restore the ranked scan position. */
	so->boolean_matches = so->boolean_results;
	so->boolean_results = NULL;
	so->result_count	= saved_count;
	so->current_pos		= saved_pos;

	return matched;
}

/*
 * Complete a combined scan with Boolean matches that have no BM25 score.
 * Ranked matches are returned first; the Boolean matches then supply the
 * zero-score tail, with the emitted-CTID set removing ranked duplicates.
 */
static bool
tp_begin_combined_boolean_tail(IndexScanDesc scan)
{
	TpScanOpaque	   so	   = (TpScanOpaque)scan->opaque;
	BufFile			  *matches = so->boolean_matches;
	TpLocalIndexState *index_state;

	so->boolean_matches = NULL;
	tp_rescan_cleanup_results(so);

	if (matches != NULL)
	{
		if (BufFileSeek(matches, 0, 0, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind BM25 Boolean result file")));

		so->boolean_results = matches;
		so->current_pos		= 0;
		return true;
	}

	index_state = tp_get_local_index_state(
			RelationGetRelid(scan->indexRelation));
	if (!index_state)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for BM25 Boolean search")));

	if (!tp_boolean_execute(scan, index_state))
	{
		so->eof_reached = true;
		return false;
	}

	return true;
}

/*
 * Get next tuple from scan
 */
bool
tp_gettuple(IndexScanDesc scan, ScanDirection dir)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;
	float4		 bm25_score;
	BlockNumber	 blknum;
	bool		 combined_scan;

	(void)dir; /* BM25 index only supports forward scan */

	Assert(scan != NULL);
	Assert(so != NULL);
	Assert(so->is_boolean_scan || so->query_text != NULL);
	combined_scan = tp_is_combined_scan(scan, so);

	/* Execute scoring query if we haven't done so yet */
	if (so->result_ctids == NULL && so->boolean_results == NULL &&
		!so->eof_reached)
	{
		/* Count index scan for pg_stat_user_indexes */
		pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
		if (scan->instrument)
			scan->instrument->nsearches++;
#endif

		if (combined_scan &&
			(so->boolean_query == NULL || so->boolean_query->size == 0))
		{
			so->eof_reached = true;
			return false;
		}
		if (so->is_boolean_scan && !combined_scan)
		{
			TpLocalIndexState *index_state = tp_get_local_index_state(
					RelationGetRelid(scan->indexRelation));

			if (!index_state)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("could not get index state for BM25 "
								"Boolean search")));

			if (!tp_boolean_execute(scan, index_state))
			{
				so->eof_reached = true;
				return false;
			}
		}
		else if (
				!tp_execute_scoring_query(scan) &&
				(!combined_scan || !tp_begin_combined_boolean_tail(scan)))
		{
			so->eof_reached = true;
			return false;
		}
		if (so->result_ctids == NULL && so->boolean_results == NULL)
		{
			so->eof_reached = true;
			return false;
		}
	}

	if (so->boolean_results != NULL)
	{
		do
		{
			if (!tp_boolean_next(scan))
			{
				so->eof_reached = true;
				return false;
			}
		} while (combined_scan && tp_ctid_seen_or_mark(so, &scan->xs_heaptid));

		scan->xs_recheck		= so->boolean_recheck;
		scan->xs_recheckorderby = false;

		if (combined_scan)
		{
			Assert(scan->numberOfOrderBys == 1);
			scan->xs_orderbyvals[0]	 = Float4GetDatum(0.0);
			scan->xs_orderbynulls[0] = false;
			tp_cached_score			 = 0.0;
		}
		return true;
	}

	/* Advance, growing the scoring batch if needed. */
	for (;;)
	{
		if (so->current_pos >= so->result_count || so->eof_reached)
		{
			/*
			 * If result_count hit the internal limit, there may be
			 * more documents.  Double the limit and re-execute the
			 * scoring query.
			 */
			if ((!so->is_boolean_scan || combined_scan) && !so->eof_reached &&
				so->result_count > 0 &&
				so->result_count >= so->max_results_used &&
				so->max_results_used < TP_MAX_QUERY_LIMIT)
			{
				int old_count = so->result_count;
				int new_limit = so->max_results_used * 2;

				/*
				 * The batch was consumed without satisfying the query, so
				 * ranked candidates are being rejected after their heap
				 * fetch.  Evaluate the Boolean query once and filter every
				 * later candidate here instead.
				 */
				if (combined_scan && so->boolean_matches == NULL &&
					!tp_boolean_filter_prepare(scan))
				{
					so->eof_reached = true;
					return false;
				}

				if (new_limit > TP_MAX_QUERY_LIMIT)
					new_limit = TP_MAX_QUERY_LIMIT;

				so->limit = new_limit;
				if (tp_execute_scoring_query(scan) &&
					so->result_count > old_count)
				{
					/*
					 * Re-scoring can reorder concurrent results, so
					 * restart and filter by emitted CTID, not position.
					 */
					so->current_pos = 0;
					continue;
				}
				else
				{
					if (combined_scan && tp_begin_combined_boolean_tail(scan))
						return tp_gettuple(scan, dir);

					so->eof_reached = true;
					return false;
				}
			}
			else
			{
				if (combined_scan && tp_begin_combined_boolean_tail(scan))
					return tp_gettuple(scan, dir);

				return false;
			}
		}

		Assert(so->scan_context != NULL);
		Assert(so->result_ctids != NULL);
		Assert(so->current_pos < so->result_count);
		Assert(ItemPointerIsValid(&so->result_ctids[so->current_pos]));

		/* Skip results with invalid block numbers */
		blknum = BlockIdGetBlockNumber(
				&(so->result_ctids[so->current_pos].ip_blkid));
		if (blknum == InvalidBlockNumber)
		{
			so->current_pos++;
			continue;
		}

		/* Reject ranked candidates the Boolean executor did not match */
		if (so->boolean_matched_ctids != NULL &&
			hash_search(
					so->boolean_matched_ctids,
					&so->result_ctids[so->current_pos],
					HASH_FIND,
					NULL) == NULL)
		{
			so->current_pos++;
			continue;
		}

		/* Skip CTIDs already emitted by an earlier pass (dedup) */
		if (tp_ctid_seen_or_mark(so, &so->result_ctids[so->current_pos]))
		{
			so->current_pos++;
			continue;
		}

		/* This position is emittable. */
		break;
	}

	scan->xs_heaptid		= so->result_ctids[so->current_pos];
	scan->xs_recheck		= so->boolean_recheck;
	scan->xs_recheckorderby = false;

	/* Set ORDER BY distance value */
	if (scan->numberOfOrderBys > 0)
	{
		float4 raw_score;

		Assert(scan->numberOfOrderBys == 1);
		Assert(scan->xs_orderbyvals != NULL);
		Assert(scan->xs_orderbynulls != NULL);
		Assert(so->result_scores != NULL);

		/* Convert BM25 score to Datum (ensure negative for ASC sort) */
		raw_score				 = so->result_scores[so->current_pos];
		bm25_score				 = (raw_score > 0) ? -raw_score : raw_score;
		scan->xs_orderbyvals[0]	 = Float4GetDatum(bm25_score);
		scan->xs_orderbynulls[0] = false;

		/* Log BM25 score if enabled */
		elog(tp_log_scores ? NOTICE : DEBUG1,
			 "BM25 index scan: tid=(%u,%u), BM25_score=%.4f",
			 BlockIdGetBlockNumber(&scan->xs_heaptid.ip_blkid),
			 scan->xs_heaptid.ip_posid,
			 bm25_score);

		/* Cache score for stub function to retrieve */
		tp_cached_score = (float8)bm25_score;
	}

	/* Move to next position */
	so->current_pos++;

	return true;
}
