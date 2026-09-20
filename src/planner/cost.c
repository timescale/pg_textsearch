/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * cost.c - Cost estimation for BM25 index scans
 */
#include <postgres.h>

#include <access/genam.h>
#include <catalog/pg_type_d.h>
#include <limits.h>
#include <nodes/pathnodes.h>
#include <nodes/primnodes.h>
#include <optimizer/optimizer.h>
#include <tsearch/ts_cache.h>
#include <tsearch/ts_type.h>
#include <tsearch/ts_utils.h>
#include <utils/float.h>
#include <utils/rel.h>
#include <utils/selfuncs.h>

#include "access/boolean.h"
#include "constants.h"
#include "index/metapage.h"
#include "planner/cost.h"

static bool
tp_boolean_get_constant_query(IndexPath *path, TSQuery *query)
{
	IndexClause *index_clause;
	OpExpr		*clause;
	Node		*query_node;
	Const		*query_const;

	index_clause = linitial_node(IndexClause, path->indexclauses);
	if (index_clause->rinfo == NULL ||
		!IsA(index_clause->rinfo->clause, OpExpr))
		return false;

	clause = castNode(OpExpr, index_clause->rinfo->clause);
	if (list_length(clause->args) != 2)
		return false;

	query_node = lsecond(clause->args);
	while (query_node != NULL && IsA(query_node, RelabelType))
		query_node = (Node *)castNode(RelabelType, query_node)->arg;

	if (query_node == NULL || !IsA(query_node, Const))
		return false;

	query_const = castNode(Const, query_node);
	if (query_const->constisnull || query_const->consttype != TSQUERYOID)
		return false;

	*query = DatumGetTSQuery(query_const->constvalue);
	return true;
}

static bool
tp_boolean_query_requires_full_scan(TSQuery query)
{
	QueryItem *items;

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

static void
tp_disable_index_path(
		IndexPath	*path,
		Cost		*indexStartupCost,
		Cost		*indexTotalCost,
		Selectivity *indexSelectivity,
		double		*indexCorrelation,
		double		*indexPages)
{
#if PG_VERSION_NUM >= 180000
	/* A fallback can add a disabled sequential scan and sort. */
	path->path.disabled_nodes += 3;
#else
	(void)path;
#endif
	*indexStartupCost = 0.0;
	*indexTotalCost	  = get_float8_infinity();
	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;
	*indexPages		  = 0.0;
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
	TSQuery			boolean_query	  = NULL;

	/*
	 * Boolean filtering and ranked scans are separate execution modes.
	 * Multiple Boolean keys and combined filtering/ranking are follow-ups.
	 */
	if ((!has_orderby && !has_boolean) || (has_orderby && has_boolean) ||
		(has_boolean && list_length(path->indexclauses) != 1))
	{
		tp_disable_index_path(
				path,
				indexStartupCost,
				indexTotalCost,
				indexSelectivity,
				indexCorrelation,
				indexPages);
		return;
	}

	if (has_boolean)
	{
		if (!tp_boolean_get_constant_query(path, &boolean_query))
			boolean_full_scan = true;
		else if (
				tp_boolean_query_exact_operand_count(boolean_query) >
				TP_BOOLEAN_MAX_EXACT_OPERANDS)
		{
			tp_disable_index_path(
					path,
					indexStartupCost,
					indexTotalCost,
					indexSelectivity,
					indexCorrelation,
					indexPages);
			return;
		}
		else
			boolean_full_scan = tp_boolean_query_requires_full_scan(
					boolean_query);
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
				tp_disable_index_path(
						path,
						indexStartupCost,
						indexTotalCost,
						indexSelectivity,
						indexCorrelation,
						indexPages);
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
	*indexTotalCost	  = boolean_full_scan
							  ? costs.indexTotalCost +
										cpu_operator_cost * num_tuples
							  : costs.indexTotalCost * TP_INDEX_SCAN_COST_FACTOR;
	*indexStartupCost = has_boolean ? *indexTotalCost
									: costs.indexStartupCost + 0.01;

	/*
	 * Calculate selectivity based on LIMIT if available, otherwise default
	 */
	if (boolean_full_scan)
	{
		*indexSelectivity = 1.0;
	}
	else if (has_boolean)
	{
		*indexSelectivity = TP_DEFAULT_INDEX_SELECTIVITY;
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
