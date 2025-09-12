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
#include <optimizer/pathnode.h>
#include <utils/guc.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>

#include "limit.h"

/*
 * Per-backend hash table for query limits - stores LIMIT values
 * extracted during query planning for use during query execution
 */
HTAB *tp_query_limits_hash = NULL;

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
	TpQueryLimitEntry *entry;
	bool			   found;

	/* Initialize per-backend hash table if needed */
	if (!tp_query_limits_hash)
	{
		HASHCTL hash_ctl;

		MemSet (&hash_ctl, 0, sizeof(hash_ctl))
			;
		hash_ctl.keysize	 = sizeof(Oid);
		hash_ctl.entrysize	 = sizeof(TpQueryLimitEntry);
		hash_ctl.hcxt		 = TopMemoryContext;
		tp_query_limits_hash = hash_create(
				"tp_query_limits",
				TP_QUERY_LIMITS_HASH_SIZE,
				&hash_ctl,
				HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	/* Find or create entry */
	entry = (TpQueryLimitEntry *)
			hash_search(tp_query_limits_hash, &index_oid, HASH_ENTER, &found);

	if (entry)
	{
		entry->index_oid = index_oid;
		entry->limit	 = limit;
	}

	elog(DEBUG1,
		 "Tapir: Stored query limit %d for index %u (per-backend)",
		 limit,
		 index_oid);
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
	TpQueryLimitEntry *entry;
	bool			   found;
	int				   result = -1;
	Oid				   index_oid;

	if (!RelationIsValid(index_rel))
		return -1;

	/* No hash table means no limits stored yet */
	if (!tp_query_limits_hash)
		return -1;

	index_oid = RelationGetRelid(index_rel);

	/* Find entry */
	entry = (TpQueryLimitEntry *)
			hash_search(tp_query_limits_hash, &index_oid, HASH_FIND, &found);

	if (found && entry)
	{
		result = entry->limit;
		elog(DEBUG1,
			 "Tapir: Retrieved query limit %d for index %u (per-backend)",
			 result,
			 index_oid);
	}

	return result;
}

/*
 * Clean up all query limit entries (called at transaction end)
 *
 * This prevents stale limit entries from affecting subsequent queries
 * in the same backend process.
 */
void
tp_cleanup_query_limits(void)
{
	HASH_SEQ_STATUS	   status;
	TpQueryLimitEntry *entry;

	/* Conservative check - only proceed if properly initialized */
	if (!tp_query_limits_hash)
		return;

	/* Additional safety check - ensure we're in a valid transaction state */
	if (!IsTransactionState())
		return;

	hash_seq_init(&status, tp_query_limits_hash);
	while ((entry = (TpQueryLimitEntry *)hash_seq_search(&status)) != NULL)
	{
		Oid index_oid = entry->index_oid;

		hash_search(tp_query_limits_hash, &index_oid, HASH_REMOVE, NULL);

		elog(DEBUG2,
			 "Tapir: Cleaned up query limit entry for index %u (per-backend)",
			 index_oid);
	}
	hash_seq_term(&status);
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
		elog(DEBUG2,
			 "Tapir: LIMIT pushdown unsafe - multiple ORDER BY clauses");
		return false;
	}

	/*
	 * For now, be conservative: don't push down LIMIT if there are
	 * additional WHERE clauses beyond the BM25 score ordering.
	 * This could be relaxed later with more sophisticated analysis.
	 */
	if (path->indexclauses && list_length(path->indexclauses) > 0)
	{
		elog(DEBUG2,
			 "Tapir: LIMIT pushdown unsafe - additional WHERE clauses "
			 "present");
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
			"tapir.default_limit",
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
