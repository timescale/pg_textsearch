/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * cost.c - Cost estimation for BM25 index scans
 */
#include <postgres.h>

#include <access/genam.h>
#include <catalog/pg_type_d.h>
#include <math.h>
#include <nodes/pathnodes.h>
#include <nodes/primnodes.h>
#include <optimizer/optimizer.h>
#include <tsearch/ts_cache.h>
#include <tsearch/ts_type.h>
#include <tsearch/ts_utils.h>
#include <utils/float.h>
#include <utils/rel.h>
#include <utils/selfuncs.h>

#include "constants.h"
#include "index/limit.h"
#include "index/metapage.h"
#include "planner/cost.h"

static bool
tp_boolean_query_requires_full_scan(IndexPath *path)
{
	IndexClause *index_clause;
	OpExpr		*clause;
	Node		*query_node;
	Const		*query_const;
	TSQuery		 query;
	QueryItem	*items;

	index_clause = linitial_node(IndexClause, path->indexclauses);
	if (index_clause->rinfo == NULL ||
		!IsA(index_clause->rinfo->clause, OpExpr))
		return true;

	clause = castNode(OpExpr, index_clause->rinfo->clause);
	if (list_length(clause->args) != 2)
		return true;

	query_node = lsecond(clause->args);
	while (query_node != NULL && IsA(query_node, RelabelType))
		query_node = (Node *)castNode(RelabelType, query_node)->arg;

	if (query_node == NULL || !IsA(query_node, Const))
		return true;

	query_const = castNode(Const, query_node);
	if (query_const->constisnull || query_const->consttype != TSQUERYOID)
		return true;

	query = DatumGetTSQuery(query_const->constvalue);
	if (query->size == 0)
		return false;

	if (!tsquery_requires_match(GETQUERY(query)))
		return true;

	items = GETQUERY(query);
	for (int i = 0; i < query->size; i++)
	{
		if (items[i].type == QI_VAL &&
			(items[i].qoperand.prefix || items[i].qoperand.weight != 0))
			return true;
	}

	return false;
}

/*
 * Seed the pushed-down internal top-K from the estimated selectivity of
 * the scanned relation's restriction clauses -- the filter ("facet")
 * that the executor applies as a Filter above the BM25 index scan.
 *
 * A filtered top-k query (WHERE <filter> ORDER BY <score> LIMIT k) is
 * planned as a BM25 top-k scan with <filter> applied above it.  If the
 * scan only produces its top k rows by score, few may satisfy the
 * Filter, forcing the executor to re-drive the scan with an
 * exponentially growing internal limit (backoff) until k rows survive --
 * and each re-drive re-scores from scratch.
 *
 * To surface k Filter-matching rows we expect to score ~k/s, where s is
 * the filter selectivity.  Seeding the internal top-K to
 * ceil(margin * k / s) up front lets a single scoring pass usually
 * suffice; the existing backoff remains the correctness safety net when
 * the estimate under-shoots.  The seed only changes scan depth, never
 * which rows win, so results are identical to the un-seeded plan.
 *
 * Returns the (possibly seeded) limit: always >= user_limit and capped
 * at TP_MAX_QUERY_LIMIT.  With seeding disabled, no restriction clauses
 * (no Filter), or a degenerate selectivity estimate, returns user_limit
 * unchanged.
 */
static int
tp_seed_limit_for_filter(PlannerInfo *root, IndexPath *path, int user_limit)
{
	RelOptInfo *rel;
	Selectivity s;
	double		seeded;

	if (!tp_filtered_seed)
		return user_limit;

	rel = path->indexinfo->rel;
	if (rel == NULL || rel->baserestrictinfo == NIL)
		return user_limit;

	/*
	 * Combined selectivity of the Filter clauses, matching how the core
	 * planner sizes the base relation (set_baserel_size_estimates passes
	 * varRelid 0 for a single base rel's baserestrictinfo).
	 */
	s = clauselist_selectivity(
			root, rel->baserestrictinfo, 0, JOIN_INNER, NULL);

	/* Only seed for a genuinely selective, non-degenerate filter. */
	if (s <= 0.0 || s >= 1.0)
		return user_limit;

	seeded = ceil(tp_filtered_seed_margin * (double)user_limit / s);
	if (seeded > (double)TP_MAX_QUERY_LIMIT)
		seeded = (double)TP_MAX_QUERY_LIMIT;

	if (seeded <= (double)user_limit)
		return user_limit;

	return (int)seeded;
}

/*
 * Estimate cost of BM25 index scan
 */
void
tp_costestimate(
		PlannerInfo *root,
		IndexPath	*path,
		double		 loop_count,
		Cost		*indexStartupCost,
		Cost		*indexTotalCost,
		Selectivity *indexSelectivity,
		double		*indexCorrelation,
		double		*indexPages)
{
	GenericCosts	costs;
	TpIndexMetaPage metap;
	double			num_tuples		  = TP_DEFAULT_TUPLE_ESTIMATE;
	bool			has_orderby		  = path->indexorderbys != NIL;
	bool			has_boolean		  = path->indexclauses != NIL;
	bool			boolean_full_scan = false;

	/*
	 * Boolean filtering and ranked scans are separate execution modes.
	 * Multiple Boolean keys and combined filtering/ranking are follow-ups.
	 */
	if ((!has_orderby && !has_boolean) || (has_orderby && has_boolean) ||
		(has_boolean && list_length(path->indexclauses) != 1))
	{
		*indexStartupCost = get_float8_infinity();
		*indexTotalCost	  = get_float8_infinity();
		return;
	}

	if (has_boolean)
		boolean_full_scan = tp_boolean_query_requires_full_scan(path);

	/* Check for LIMIT clause and verify it can be safely pushed down */
	if (has_orderby && root && root->limit_tuples > 0 &&
		root->limit_tuples < INT_MAX)
	{
		int limit = (int)root->limit_tuples;

		if (tp_can_pushdown_limit(root, path, limit))
		{
			/*
			 * Seed the internal top-K from the estimated selectivity of
			 * any Filter above this scan, so filtered top-k queries
			 * avoid the executor's backoff re-drives (no-op when there
			 * is no filter).
			 *
			 * NOTE: tp_store_query_limit uses a single per-backend slot
			 * keyed only by index_oid, so multiple BM25 scans of the
			 * SAME index in one statement (e.g. a faceted UNION ALL)
			 * share it and may not each receive their own seed.  This
			 * is a pre-existing limitation of the limit stash;
			 * correctness is unaffected (executor Filter + backoff).
			 * Tracked in #435.
			 */
			int seeded = tp_seed_limit_for_filter(root, path, limit);

			tp_store_query_limit(path->indexinfo->indexoid, seeded);
		}
	}

	/* Try to get actual statistics from the index */
	if (path->indexinfo && path->indexinfo->indexoid != InvalidOid)
	{
		Relation index_rel =
				index_open(path->indexinfo->indexoid, AccessShareLock);

		if (index_rel)
		{
			metap = tp_get_metapage(index_rel);
			if (has_boolean &&
				metap->text_config_oid != getTSCurrentConfig(true))
			{
				pfree(metap);
				index_close(index_rel, AccessShareLock);
				*indexStartupCost = get_float8_infinity();
				*indexTotalCost	  = get_float8_infinity();
				return;
			}

			if (metap && metap->total_docs > 0)
				num_tuples = (double)metap->total_docs;

			if (metap)
				pfree(metap);

			index_close(index_rel, AccessShareLock);
		}
	}

	/* Initialize generic costs */
	MemSet (&costs, 0, sizeof(costs))
		;
	genericcostestimate(root, path, loop_count, &costs);

	/* Override with BM25-specific estimates */
	*indexStartupCost = costs.indexStartupCost + 0.01;
	*indexTotalCost	  = boolean_full_scan
							  ? costs.indexTotalCost +
										cpu_operator_cost * num_tuples
							  : costs.indexTotalCost * TP_INDEX_SCAN_COST_FACTOR;

	/*
	 * Calculate selectivity based on LIMIT if available, otherwise default
	 */
	if (boolean_full_scan)
	{
		*indexSelectivity = 1.0;
	}
	else if (
			root && root->limit_tuples > 0 && root->limit_tuples < INT_MAX &&
			num_tuples > 0)
	{
		/* Use LIMIT as upper bound for selectivity calculation */
		double limit_selectivity = Min(1.0, root->limit_tuples / num_tuples);
		*indexSelectivity =
				Max(limit_selectivity, TP_DEFAULT_INDEX_SELECTIVITY);
	}
	else
	{
		*indexSelectivity = TP_DEFAULT_INDEX_SELECTIVITY;
	}
	*indexCorrelation = 0.0; /* No correlation assumptions */
	*indexPages		  = Max(1.0, num_tuples / 100.0); /* Rough page estimate */
}
