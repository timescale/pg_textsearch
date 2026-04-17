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
#include <pgstat.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/regproc.h>
#include <utils/rel.h>

#include "access/am.h"
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

float8
tp_get_cached_score(void)
{
	return tp_cached_score;
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

	scan = RelationGetIndexScan(index, nkeys, norderbys);

	/* Allocate and initialize scan opaque data */
	so				 = (TpScanOpaque)palloc0(sizeof(TpScanOpaqueData));
	so->scan_context = AllocSetContextCreate(
			CurrentMemoryContext,
			"Tapir Scan Context",
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
		ScanKey		  keys __attribute__((unused)),
		int			  nkeys __attribute__((unused)),
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

		/* Reset scan position and state */
		so->current_pos	 = 0;
		so->result_count = 0;
		so->eof_reached	 = false;
		so->query_vector = NULL;
	}

	/* Process ORDER BY scan keys for <@> operator */
	if (norderbys > 0 && orderbys && so)
	{
		/* Get index metadata to check if we have documents */
		if (!metap)
			metap = tp_get_metapage(scan->indexRelation);

		tp_rescan_process_orderby(scan, orderbys, norderbys, metap);

		if (metap)
			pfree(metap);
	}
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
 * Get next tuple from scan
 */
bool
tp_gettuple(IndexScanDesc scan, ScanDirection dir)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;
	float4		 bm25_score;
	BlockNumber	 blknum;

	(void)dir; /* BM25 index only supports forward scan */

	Assert(scan != NULL);
	Assert(so != NULL);
	Assert(so->query_text != NULL);

	/* Execute scoring query if we haven't done so yet */
	if (so->result_ctids == NULL && !so->eof_reached)
	{
		/* Count index scan for pg_stat_user_indexes */
		pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
		if (scan->instrument)
			scan->instrument->nsearches++;
#endif

		if (!tp_execute_scoring_query(scan))
		{
			so->eof_reached = true;
			return false;
		}
		/* Scoring query must have allocated result_ctids on success */
		if (so->result_ctids == NULL)
		{
			so->eof_reached = true;
			return false;
		}
	}

	/* Check if we've reached the end of current result set */
	if (so->current_pos >= so->result_count || so->eof_reached)
	{
		/*
		 * If result_count hit the internal limit, there may be more
		 * documents.  Double the limit and re-execute the scoring
		 * query, skipping already-returned results.
		 */
		if (!so->eof_reached && so->result_count > 0 &&
			so->result_count >= so->max_results_used &&
			so->max_results_used < TP_MAX_QUERY_LIMIT)
		{
			int old_count = so->result_count;
			int new_limit = so->max_results_used * 2;

			if (new_limit > TP_MAX_QUERY_LIMIT)
				new_limit = TP_MAX_QUERY_LIMIT;

			so->limit = new_limit;
			if (tp_execute_scoring_query(scan) && so->result_count > old_count)
			{
				/* Skip already-returned results */
				so->current_pos = old_count;
				/* Fall through to return next tuple */
			}
			else
			{
				so->eof_reached = true;
				return false;
			}
		}
		else
			return false;
	}

	Assert(so->scan_context != NULL);
	Assert(so->result_ctids != NULL);
	Assert(so->current_pos < so->result_count);

	Assert(ItemPointerIsValid(&so->result_ctids[so->current_pos]));

	/* Skip results with invalid block numbers */
	blknum = BlockIdGetBlockNumber(
			&(so->result_ctids[so->current_pos].ip_blkid));
	while (blknum == InvalidBlockNumber)
	{
		so->current_pos++;
		if (so->current_pos >= so->result_count)
			return false;
		blknum = BlockIdGetBlockNumber(
				&(so->result_ctids[so->current_pos].ip_blkid));
	}

	scan->xs_heaptid		= so->result_ctids[so->current_pos];
	scan->xs_recheck		= false;
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
