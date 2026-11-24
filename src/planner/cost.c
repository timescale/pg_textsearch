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
#include <miscadmin.h>
#include <nodes/nodeFuncs.h>
#include <nodes/pathnodes.h>
#include <nodes/primnodes.h>
#include <optimizer/optimizer.h>
#include <tsearch/ts_cache.h>
#include <tsearch/ts_type.h>
#include <tsearch/ts_utils.h>
#include <utils/float.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/selfuncs.h>

#include "access/boolean.h"
#include "constants.h"
#include "index/metapage.h"
#include "planner/cost.h"
#include "planner/hooks.h"

static bool
tp_boolean_get_constant_query(IndexClause *index_clause, TSQuery *query)
{
	OpExpr *clause;
	Node   *query_node;
	Const  *query_const;

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
tp_boolean_query_has_inexact_operand(TSQuery query)
{
	QueryItem *items = GETQUERY(query);

	for (int i = 0; i < query->size; i++)
	{
		if (items[i].type == QI_VAL &&
			(items[i].qoperand.prefix || items[i].qoperand.weight != 0))
			return true;
	}

	return false;
}

static bool
tp_boolean_query_requires_full_scan(TSQuery query)
{
	if (query->size == 0)
		return false;

	return !tsquery_requires_match(GETQUERY(query)) ||
		   tp_boolean_query_has_inexact_operand(query);
}

/* Whether every match must contain some exact (unweighted, whole) term. */
static bool
tp_boolean_requires_exact_operand(QueryItem *item)
{
	check_stack_depth();

	if (item->type == QI_VAL)
		return !item->qoperand.prefix && item->qoperand.weight == 0;

	switch (item->qoperator.oper)
	{
	case OP_AND:
	case OP_PHRASE:
		return tp_boolean_requires_exact_operand(
					   item + item->qoperator.left) ||
			   tp_boolean_requires_exact_operand(item + 1);
	case OP_OR:
		return tp_boolean_requires_exact_operand(
					   item + item->qoperator.left) &&
			   tp_boolean_requires_exact_operand(item + 1);
	default:
		return false;
	}
}

static bool
tp_has_ranking_operator(Node *node, void *context)
{
	IndexOptInfo *index = context;

	if (node == NULL || IsA(node, Query))
		return false;
	if (IsA(node, OpExpr))
	{
		OpExpr *expr = castNode(OpExpr, node);

		for (int i = 0; i < index->nkeycolumns; i++)
		{
			if (OidIsValid(get_op_opfamily_sortfamily(
						expr->opno, index->opfamily[i])))
				return true;
		}
	}
	return expression_tree_walker(node, tp_has_ranking_operator, context);
}

static bool
tp_query_has_ranked_sort(PlannerInfo *root, IndexPath *path)
{
	ListCell *lc;

	if (root == NULL || root->parse == NULL || path->indexinfo == NULL)
		return false;
	foreach (lc, root->parse->sortClause)
	{
		SortGroupClause *clause = lfirst_node(SortGroupClause, lc);
		Node *expr = get_sortgroupclause_expr(clause, root->parse->targetList);

		if (tp_has_ranking_operator(expr, path->indexinfo))
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

	if ((!has_orderby && !has_boolean) ||
		(has_boolean && has_orderby &&
		 (!path->indexinfo || !OidIsValid(path->indexinfo->indexoid))) ||
		(has_orderby && tp_indexpath_requires_other_index(path)))
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
		ListCell *lc;
		int		  exact_operands = 0;
		bool	  runtime_key	 = false;
		bool	  inexact		 = false;
		bool	  exact_filter	 = false;

		foreach (lc, path->indexclauses)
		{
			TSQuery boolean_query = NULL;

			if (!tp_boolean_get_constant_query(
						lfirst_node(IndexClause, lc), &boolean_query))
			{
				/*
				 * Unordered scans reject runtime keys over the operand cap.
				 * Combined keys have no plan-time bound, and ranked queries
				 * have an ordered path that rechecks instead.
				 */
				if (!has_orderby && (list_length(path->indexclauses) > 1 ||
									 tp_query_has_ranked_sort(root, path)))
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
				runtime_key		  = true;
				boolean_full_scan = true;
			}
			else
			{
				int operands = tp_boolean_query_exact_operand_count(
						boolean_query);

				/* Ranked scans cannot filter over-budget predicates either. */
				if (operands > TP_BOOLEAN_MAX_EXACT_OPERANDS - exact_operands)
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
				exact_operands += operands;
				boolean_full_scan |= tp_boolean_query_requires_full_scan(
						boolean_query);
				inexact |= tp_boolean_query_has_inexact_operand(boolean_query);
				exact_filter |= boolean_query->size == 0 ||
								tp_boolean_requires_exact_operand(
										GETQUERY(boolean_query));
			}
		}

		/*
		 * Ranked scans filter BMW candidates on exact terms only.  Unless
		 * every match contains one, prefix and weighted operands leave each
		 * candidate to the heap recheck, which rescans ever deeper.
		 */
		if (has_orderby && inexact && !exact_filter && !runtime_key)
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
	}

	/* Ranked scans evaluate the predicate on BMW candidates, not every doc. */
	boolean_full_scan = boolean_full_scan && !has_orderby;

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
	*indexStartupCost = has_orderby ? costs.indexStartupCost + 0.01
									: *indexTotalCost;

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

	/*
	 * The selectivities above are fractions of the index's documents, but
	 * cost_index() applies indexSelectivity to the heap.  A partial index
	 * holds only part of the heap, so scale to the heap's tuple count.
	 */
	if (path->indexinfo->rel->tuples > num_tuples)
		*indexSelectivity *= num_tuples / path->indexinfo->rel->tuples;

	*indexCorrelation = 0.0; /* No correlation assumptions */
	*indexPages		  = Max(1.0, num_tuples / 100.0); /* Rough page estimate */
}
