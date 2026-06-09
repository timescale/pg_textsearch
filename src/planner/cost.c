/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * cost.c - Cost estimation for BM25 index scans
 */
#include <postgres.h>

#include <access/genam.h>
#include <nodes/nodeFuncs.h>
#include <optimizer/optimizer.h>
#include <parser/parsetree.h>
#include <utils/float.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/selfuncs.h>

#include "constants.h"
#include "index/facet.h"
#include "index/limit.h"
#include "index/metapage.h"
#include "planner/cost.h"

/*
 * Detect a faceted-search pushdown opportunity for a BM25 index path.
 *
 * Looks for a single "Var OP Const" restriction clause on the scanned
 * relation (e.g. category = 'engineering') and, if it is selective enough,
 * stashes a facet spec so the executor can constrain BMW to matching rows.
 *
 * Kept deliberately simple: only one clause is pushed down. Any additional
 * clauses (and the exact recheck of this one) are handled by PostgreSQL's
 * Filter node above the index scan, so a superset allow-list is always safe.
 */
static void
tp_try_store_facet(PlannerInfo *root, IndexPath *path)
{
	RelOptInfo *rel;
	ListCell   *lc;

	if (!tp_enable_facet_pushdown || root == NULL)
		return;

	rel = path->indexinfo->rel;
	if (rel == NULL || rel->baserestrictinfo == NIL || rel->tuples <= 0)
		return;

	foreach (lc, rel->baserestrictinfo)
	{
		RestrictInfo  *rinfo = (RestrictInfo *)lfirst(lc);
		OpExpr		  *opexpr;
		Node		  *leftop;
		Node		  *rightop;
		Var			  *var;
		Const		  *con;
		bool		   var_on_left;
		Selectivity	   selec;
		int16		   typlen;
		bool		   typbyval;
		RangeTblEntry *rte;

		if (!IsA(rinfo->clause, OpExpr))
			continue;

		opexpr = (OpExpr *)rinfo->clause;
		if (list_length(opexpr->args) != 2)
			continue;

		leftop	= (Node *)linitial(opexpr->args);
		rightop = (Node *)lsecond(opexpr->args);

		/* Strip relabel nodes that wrap binary-compatible casts. */
		leftop	= strip_implicit_coercions(leftop);
		rightop = strip_implicit_coercions(rightop);

		if (IsA(leftop, Var) && IsA(rightop, Const))
		{
			var			= (Var *)leftop;
			con			= (Const *)rightop;
			var_on_left = true;
		}
		else if (IsA(leftop, Const) && IsA(rightop, Var))
		{
			con			= (Const *)leftop;
			var			= (Var *)rightop;
			var_on_left = false;
		}
		else
			continue;

		/* Must reference this base relation, a real (positive) column. */
		if ((Index)var->varno != rel->relid || var->varlevelsup != 0 ||
			var->varattno <= 0)
			continue;

		if (con->constisnull)
			continue;

		/* Selectivity gate: only push down reasonably selective facets. */
		selec = clause_selectivity(root, (Node *)rinfo, 0, JOIN_INNER, NULL);
		if (selec < 0.0 || selec > tp_facet_selectivity_threshold)
			continue;

		rte = planner_rt_fetch(rel->relid, root);
		if (rte == NULL || rte->rtekind != RTE_RELATION)
			continue;

		get_typlenbyval(con->consttype, &typlen, &typbyval);

		tp_store_query_facet(
				path->indexinfo->indexoid,
				rte->relid,
				var->varattno,
				opexpr->opno,
				opexpr->inputcollid,
				var_on_left,
				con->constvalue,
				typlen,
				typbyval);

		/* Only the first usable clause is pushed down. */
		return;
	}
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
	double			num_tuples = TP_DEFAULT_TUPLE_ESTIMATE;

	/* Never use index without ORDER BY clause */
	if (!path->indexorderbys || list_length(path->indexorderbys) == 0)
	{
		*indexStartupCost = get_float8_infinity();
		*indexTotalCost	  = get_float8_infinity();
		return;
	}

	/* Check for LIMIT clause and verify it can be safely pushed down */
	if (root && root->limit_tuples > 0 && root->limit_tuples < INT_MAX)
	{
		int limit = (int)root->limit_tuples;

		if (tp_can_pushdown_limit(root, path, limit))
			tp_store_query_limit(path->indexinfo->indexoid, limit);
	}

	/* Detect a faceted-search (scalar filter) pushdown opportunity. */
	tp_try_store_facet(root, path);

	/* Try to get actual statistics from the index */
	if (path->indexinfo && path->indexinfo->indexoid != InvalidOid)
	{
		Relation index_rel =
				index_open(path->indexinfo->indexoid, AccessShareLock);

		if (index_rel)
		{
			metap = tp_get_metapage(index_rel);
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
	*indexStartupCost = costs.indexStartupCost + 0.01; /* Small startup cost */
	*indexTotalCost	  = costs.indexTotalCost * TP_INDEX_SCAN_COST_FACTOR;

	/*
	 * Calculate selectivity based on LIMIT if available, otherwise default
	 */
	if (root && root->limit_tuples > 0 && root->limit_tuples < INT_MAX &&
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
