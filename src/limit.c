/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */

/*
 * limits.c - Tapir Query LIMIT Optimization
 *
 * This module implements LIMIT pushdown optimization for Tapir BM25 queries.
 * When PostgreSQL queries have LIMIT clauses with ORDER BY BM25 scores,
 * we can optimize by computing only the top N results.
 *
 * IDENTIFICATION
 *	  src/limits.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/xact.h>
#include <nodes/pathnodes.h>
#include <utils/guc.h>

#include "limit.h"

/*
 * Per-backend structure for current query limit - stores LIMIT value
 * extracted during query planning for use during query execution
 */
static TpCurrentLimit tp_current_limit = {InvalidOid, -1, false};

/*
 * Default limit when no LIMIT clause is detected - prevents
 * unbounded result sets that could consume excessive memory
 */
int tp_default_limit = TP_DEFAULT_QUERY_LIMIT;

/*
 * Store a query limit for a specific index and backend
 *
 * This is called during query planning (tp_costestimate) when we detect
 * a safe LIMIT pushdown opportunity. The stored limit is later retrieved
 * during query execution.
 */
void
tp_store_query_limit(Oid index_oid, int limit)
{
	/* Safety check - warn if we're overwriting a different index's limit */
	if (tp_current_limit.is_valid && tp_current_limit.index_oid != index_oid)
	{
	}

	/* Store the limit in our simple structure */
	tp_current_limit.index_oid = index_oid;
	tp_current_limit.limit	   = limit;
	tp_current_limit.is_valid  = true;
}

/*
 * Get the query limit for a specific index and current backend
 *
 * Returns the stored limit value, or -1 if no limit was stored.
 * This is called during query execution to check if LIMIT optimization
 * should be applied.
 */
int
tp_get_query_limit(Relation index_rel)
{
	Oid index_oid;
	int result = -1;

	if (!RelationIsValid(index_rel))
		return -1;

	/* Check if we have valid limit data */
	if (!tp_current_limit.is_valid)
		return -1;

	index_oid = RelationGetRelid(index_rel);

	/* Check if the stored limit applies to this index */
	if (tp_current_limit.index_oid == index_oid)
	{
		result = tp_current_limit.limit;

		/* Clear the limit after retrieval to prevent stale data */
		tp_current_limit.is_valid = false;
	}

	return result;
}

/*
 * Clean up query limit data (called at transaction end)
 *
 * This prevents stale limit entries from affecting subsequent queries
 * in the same backend process.
 */
void
tp_cleanup_query_limits(void)
{
	/* Additional safety check - ensure we're in a valid transaction state */
	if (!IsTransactionState())
		return;

	/* Clear the current limit structure */
	if (tp_current_limit.is_valid)
	{
		tp_current_limit.index_oid = InvalidOid;
		tp_current_limit.limit	   = -1;
		tp_current_limit.is_valid  = false;
	}
}

/*
 * Analyze whether LIMIT pushdown is safe for the given query path
 *
 * LIMIT pushdown is only safe when:
 * 1. The index scan produces results in the same order as the query's ORDER BY
 * 2. There are no intervening operations that could reorder results
 * 3. We have exactly one ORDER BY clause (our BM25 score)
 * 4. No additional WHERE clauses that might interfere with ordering
 */
bool
tp_can_pushdown_limit(PlannerInfo *root, IndexPath *path, int limit)
{
	/* Basic validation */
	if (!root || !path || limit <= 0)
		return false;

	/*
	 * Must have exactly one ORDER BY clause - our BM25 score.
	 * Multiple ORDER BY clauses could affect result ordering.
	 */
	if (!path->indexorderbys || list_length(path->indexorderbys) != 1)
	{
		return false;
	}

	/*
	 * For now, be conservative: don't push down LIMIT if there are
	 * additional WHERE clauses beyond the BM25 score ordering.
	 * This could be relaxed later with more sophisticated analysis.
	 */
	if (path->indexclauses && list_length(path->indexclauses) > 0)
	{
		return false;
	}

	return true;
}

/*
 * Initialize GUC parameters for the limits module
 *
 * Called from _PG_init() in mod.c during extension initialization
 */
void
tp_limits_init(void)
{
	DefineCustomIntVariable(
			"pg_textsearch.default_limit",
			"Default limit for BM25 queries when no LIMIT is detected",
			"Controls the maximum number of documents to process when no "
			"LIMIT "
			"clause is present",
			&tp_default_limit,
			TP_DEFAULT_QUERY_LIMIT, /* default 1000 */
			1,						/* min 1 */
			TP_MAX_QUERY_LIMIT,		/* max 100k */
			PGC_USERSET,
			0,
			NULL,
			NULL,
			NULL);
}
