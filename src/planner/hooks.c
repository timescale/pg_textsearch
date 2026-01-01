/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * planner/hooks.c - Parse analysis and planner hooks
 *
 * When queries use the <@> operator without an explicit index, this hook
 * identifies the column being scored and finds a suitable BM25 index.
 * It also optimizes BM25 score expressions in the planner.
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/htup_details.h>
#include <access/table.h>
#include <catalog/indexing.h>
#include <catalog/namespace.h>
#include <catalog/pg_am.h>
#include <catalog/pg_index.h>
#include <catalog/pg_opclass.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <catalog/pg_type_d.h>
#include <commands/defrem.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/plannodes.h>
#include <optimizer/optimizer.h>
#include <optimizer/planner.h>
#include <parser/analyze.h>
#include <parser/parse_func.h>
#include <parser/parse_oper.h>
#include <parser/parsetree.h>
#include <utils/builtins.h>
#include <utils/catcache.h>
#include <utils/fmgroids.h>
#include <utils/inval.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/syscache.h>

#include "hooks.h"
#include "query/score.h"
#include "types/query.h"

/* Previous hooks in chain */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static planner_hook_type			prev_planner_hook			 = NULL;

/*
 * Structure to hold looked-up OIDs for a query
 */
typedef struct BM25OidCache
{
	Oid bm25_am_oid;
	Oid tpquery_type_oid;
	Oid text_tpquery_operator_oid;
	Oid text_text_operator_oid;
} BM25OidCache;

/*
 * Backend-local cache to avoid repeated syscache lookups.
 * Cache is invalidated when pg_am changes (extension install/drop).
 */
typedef enum
{
	CACHE_UNKNOWN,	 /* Not yet checked */
	CACHE_NOT_FOUND, /* Extension not installed */
	CACHE_VALID		 /* Extension installed, OIDs cached */
} CacheState;

static CacheState	oid_cache_state = CACHE_UNKNOWN;
static BM25OidCache cached_oids;
static bool			invalidation_registered = false;

/*
 * Context for query tree mutation
 */
typedef struct ResolveIndexContext
{
	Query		 *query;
	BM25OidCache *oid_cache;
} ResolveIndexContext;

/*
 * Syscache invalidation callback - reset cache when pg_am or pg_type changes.
 */
static void
bm25_cache_invalidation_callback(
		Datum arg		 pg_attribute_unused(),
		int cacheid		 pg_attribute_unused(),
		uint32 hashvalue pg_attribute_unused())
{
	oid_cache_state = CACHE_UNKNOWN;
}

/*
 * Look up OIDs for BM25 extension objects (uncached).
 * Returns false if extension is not installed.
 */
static bool
lookup_bm25_oids_internal(BM25OidCache *cache)
{
	HeapTuple tuple;
	List	 *opname;

	memset(cache, 0, sizeof(BM25OidCache));

	/* Look up bm25 access method */
	tuple = SearchSysCache1(AMNAME, CStringGetDatum("bm25"));
	if (HeapTupleIsValid(tuple))
	{
		cache->bm25_am_oid = ((Form_pg_am)GETSTRUCT(tuple))->oid;
		ReleaseSysCache(tuple);
	}

	if (!OidIsValid(cache->bm25_am_oid))
		return false;

	/* Look up bm25query type using the search path */
	cache->tpquery_type_oid = TypenameGetTypid("bm25query");
	if (!OidIsValid(cache->tpquery_type_oid))
		return false;

	/* Look up the <@> operator for (text, bm25query) */
	opname = list_make1(makeString("<@>"));
	cache->text_tpquery_operator_oid =
			OpernameGetOprid(opname, TEXTOID, cache->tpquery_type_oid);
	list_free(opname);

	/* Look up the <@> operator for (text, text) */
	opname						  = list_make1(makeString("<@>"));
	cache->text_text_operator_oid = OpernameGetOprid(opname, TEXTOID, TEXTOID);
	list_free(opname);

	return true;
}

/*
 * Get BM25 OIDs with caching.
 * Returns false if extension is not installed.
 */
static bool
get_bm25_oids(BM25OidCache *cache)
{
	/* Register invalidation callbacks on first call */
	if (!invalidation_registered)
	{
		CacheRegisterSyscacheCallback(
				AMOID, bm25_cache_invalidation_callback, (Datum)0);
		CacheRegisterSyscacheCallback(
				TYPEOID, bm25_cache_invalidation_callback, (Datum)0);
		invalidation_registered = true;
	}

	/* Fast path: use cached result */
	switch (oid_cache_state)
	{
	case CACHE_NOT_FOUND:
		return false;
	case CACHE_VALID:
		*cache = cached_oids;
		return true;
	case CACHE_UNKNOWN:
		break;
	}

	/* Do the actual lookup */
	if (!lookup_bm25_oids_internal(cache))
	{
		oid_cache_state = CACHE_NOT_FOUND;
		return false;
	}

	cached_oids		= *cache;
	oid_cache_state = CACHE_VALID;
	return true;
}

/*
 * Find BM25 index for a column.
 */
static Oid
find_bm25_index_for_column(Oid relid, AttrNumber attnum, Oid bm25_am_oid)
{
	Relation	indexRelation;
	SysScanDesc scan;
	HeapTuple	indexTuple;
	Oid			result		= InvalidOid;
	int			index_count = 0;
	ScanKeyData scanKey;

	if (!OidIsValid(bm25_am_oid))
		return InvalidOid;

	indexRelation = table_open(IndexRelationId, AccessShareLock);

	ScanKeyInit(
			&scanKey,
			Anum_pg_index_indrelid,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(relid));

	scan = systable_beginscan(
			indexRelation, IndexIndrelidIndexId, true, NULL, 1, &scanKey);

	while ((indexTuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_index indexForm = (Form_pg_index)GETSTRUCT(indexTuple);
		Oid			  indexOid	= indexForm->indexrelid;
		HeapTuple	  classTuple;
		Form_pg_class classForm;

		classTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(indexOid));
		if (!HeapTupleIsValid(classTuple))
			continue;

		classForm = (Form_pg_class)GETSTRUCT(classTuple);

		if (classForm->relam == bm25_am_oid)
		{
			int nkeys = indexForm->indnatts;

			for (int i = 0; i < nkeys; i++)
			{
				if (indexForm->indkey.values[i] == attnum)
				{
					index_count++;
					if (result == InvalidOid)
						result = indexOid;
					break;
				}
			}
		}
		ReleaseSysCache(classTuple);
	}

	systable_endscan(scan);
	table_close(indexRelation, AccessShareLock);

	if (index_count > 1)
		ereport(WARNING,
				(errmsg("multiple BM25 indexes exist on the same column"),
				 errhint("Use explicit to_bm25query('query', 'index_name') "
						 "to specify which index to use.")));

	return result;
}

/*
 * Extract relation OID and attribute number from a Var node.
 */
static bool
get_var_relation_and_attnum(
		Var *var, Query *query, Oid *relid, AttrNumber *attnum)
{
	RangeTblEntry *rte;

	if (var->varno < 1 || var->varno > list_length(query->rtable))
		return false;

	rte = rt_fetch(var->varno, query->rtable);
	if (rte->rtekind != RTE_RELATION)
		return false;

	*relid	= rte->relid;
	*attnum = var->varattno;
	return true;
}

/*
 * Find BM25 index OID from a Var node representing the indexed column.
 */
static Oid
find_index_for_var(Var *var, ResolveIndexContext *context)
{
	Oid		   relid;
	AttrNumber attnum;

	if (!get_var_relation_and_attnum(var, context->query, &relid, &attnum))
		return InvalidOid;

	return find_bm25_index_for_column(
			relid, attnum, context->oid_cache->bm25_am_oid);
}

/*
 * Create a new tpquery Const with the resolved index OID.
 */
static Const *
create_resolved_tpquery_const(Const *original, Oid index_oid)
{
	TpQuery *old_tpquery = (TpQuery *)DatumGetPointer(original->constvalue);
	char	*query_text	 = get_tpquery_text(old_tpquery);
	TpQuery *new_tpquery = create_tpquery(query_text, index_oid);

	return makeConst(
			original->consttype,
			original->consttypmod,
			original->constcollid,
			VARSIZE(new_tpquery),
			PointerGetDatum(new_tpquery),
			false,
			false);
}

/*
 * Create a new OpExpr with the given arguments.
 */
static OpExpr *
create_opexpr(Oid opno, Node *left, Node *right, Oid inputcollid, int location)
{
	OpExpr *new_opexpr = makeNode(OpExpr);

	new_opexpr->opno		 = opno;
	new_opexpr->opfuncid	 = get_opcode(opno);
	new_opexpr->opresulttype = FLOAT8OID;
	new_opexpr->opretset	 = false;
	new_opexpr->opcollid	 = InvalidOid;
	new_opexpr->inputcollid	 = inputcollid;
	new_opexpr->args		 = list_make2(copyObject(left), right);
	new_opexpr->location	 = location;

	return new_opexpr;
}

/*
 * Transform text <@> bm25query with unresolved index.
 * Returns transformed OpExpr or NULL if no transformation needed.
 */
static Node *
transform_tpquery_opexpr(OpExpr *opexpr, ResolveIndexContext *context)
{
	BM25OidCache *oids = context->oid_cache;
	Node		 *left;
	Node		 *right;
	Const		 *constNode;
	TpQuery		 *tpquery;
	Oid			  index_oid;
	Const		 *new_const;

	if (opexpr->opno != oids->text_tpquery_operator_oid)
		return NULL;
	if (list_length(opexpr->args) != 2)
		return NULL;

	left  = linitial(opexpr->args);
	right = lsecond(opexpr->args);

	/* Try to simplify function calls (e.g., to_bm25query()) to constants */
	if (IsA(right, FuncExpr))
		right = eval_const_expressions(NULL, right);

	if (!IsA(right, Const))
		return NULL;

	constNode = (Const *)right;
	if (constNode->consttype != oids->tpquery_type_oid ||
		constNode->constisnull)
		return NULL;

	tpquery = (TpQuery *)DatumGetPointer(constNode->constvalue);
	if (OidIsValid(tpquery->index_oid))
		return NULL; /* Already resolved */

	if (!IsA(left, Var))
		return NULL;

	index_oid = find_index_for_var((Var *)left, context);
	if (!OidIsValid(index_oid))
		return NULL;

	new_const = create_resolved_tpquery_const(constNode, index_oid);
	return (Node *)create_opexpr(
			opexpr->opno,
			left,
			(Node *)new_const,
			opexpr->inputcollid,
			opexpr->location);
}

/*
 * Transform text <@> text to text <@> bm25query.
 * Returns transformed OpExpr or NULL if no transformation needed.
 */
static Node *
transform_text_text_opexpr(OpExpr *opexpr, ResolveIndexContext *context)
{
	BM25OidCache *oids = context->oid_cache;
	Node		 *left;
	Node		 *right;
	Var			 *var;
	Const		 *text_const;
	Oid			  index_oid;
	char		 *query_text;
	TpQuery		 *tpquery;
	Const		 *tpquery_const;

	if (opexpr->opno != oids->text_text_operator_oid)
		return NULL;
	if (list_length(opexpr->args) != 2)
		return NULL;

	left  = linitial(opexpr->args);
	right = lsecond(opexpr->args);

	if (!IsA(left, Var) || !IsA(right, Const))
		return NULL;

	var		   = (Var *)left;
	text_const = (Const *)right;

	if (text_const->consttype != TEXTOID || text_const->constisnull)
		return NULL;

	index_oid = find_index_for_var(var, context);
	if (!OidIsValid(index_oid))
		return NULL;

	query_text = TextDatumGetCString(text_const->constvalue);
	tpquery	   = create_tpquery(query_text, index_oid);
	pfree(query_text);

	tpquery_const = makeConst(
			oids->tpquery_type_oid,
			-1,
			InvalidOid,
			-1, /* varlena type */
			PointerGetDatum(tpquery),
			false,
			false);

	return (Node *)create_opexpr(
			oids->text_tpquery_operator_oid,
			left,
			(Node *)tpquery_const,
			opexpr->inputcollid,
			opexpr->location);
}

/*
 * Mutator function to resolve unresolved tpquery constants.
 */
static Node *
resolve_index_mutator(Node *node, ResolveIndexContext *context)
{
	Node *result;

	if (node == NULL)
		return NULL;

	if (IsA(node, OpExpr))
	{
		OpExpr *opexpr = (OpExpr *)node;

		/* Try text <@> bm25query transformation */
		result = transform_tpquery_opexpr(opexpr, context);
		if (result)
			return result;

		/* Try text <@> text transformation */
		result = transform_text_text_opexpr(opexpr, context);
		if (result)
			return result;
	}

	return expression_tree_mutator(node, resolve_index_mutator, context);
}

/*
 * Process a single query's target list.
 */
static void
resolve_indexes_in_targetlist(Query *query, ResolveIndexContext *context)
{
	ListCell *lc;

	if (!query->targetList)
		return;

	foreach (lc, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(lc);

		tle->expr = (Expr *)resolve_index_mutator((Node *)tle->expr, context);
	}
}

/* Forward declaration */
static void resolve_indexes_in_query(Query *query);

/*
 * Process subqueries in CTEs and FROM clause.
 */
static void
resolve_indexes_in_subqueries(Query *query)
{
	ListCell *lc;

	/* Process CTEs */
	foreach (lc, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *)lfirst(lc);

		if (cte->ctequery && IsA(cte->ctequery, Query))
			resolve_indexes_in_query((Query *)cte->ctequery);
	}

	/* Process FROM subqueries */
	foreach (lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *)lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && rte->subquery)
			resolve_indexes_in_query(rte->subquery);
	}
}

static void
resolve_indexes_in_query(Query *query)
{
	ResolveIndexContext context;
	BM25OidCache		oid_cache;

	if (!get_bm25_oids(&oid_cache))
		return;

	context.query	  = query;
	context.oid_cache = &oid_cache;

	/* Process target list */
	resolve_indexes_in_targetlist(query, &context);

	/* Process WHERE clause */
	if (query->jointree && query->jointree->quals)
		query->jointree->quals =
				resolve_index_mutator(query->jointree->quals, &context);

	/* Process HAVING clause */
	if (query->havingQual)
		query->havingQual = resolve_index_mutator(query->havingQual, &context);

	/* Process subqueries */
	resolve_indexes_in_subqueries(query);
}

/*
 * Post parse analysis hook function.
 */
static void
tp_post_parse_analyze_hook(
		ParseState *pstate	pg_attribute_unused(),
		Query			   *query,
		JumbleState *jstate pg_attribute_unused())
{
	resolve_indexes_in_query(query);

	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);
}

/* ============================================================================
 * Planner hook: Replace BM25 score expressions with stub function
 * ============================================================================
 */

/*
 * Check if an expression is our BM25 scoring operator (text <@> bm25query).
 */
static bool
is_bm25_score_opexpr(Node *node, BM25OidCache *oids)
{
	OpExpr *opexpr;

	if (!IsA(node, OpExpr))
		return false;

	opexpr = (OpExpr *)node;
	return opexpr->opno == oids->text_tpquery_operator_oid;
}

/*
 * Create a FuncExpr calling bm25_get_current_score().
 */
static FuncExpr *
make_stub_funcexpr(void)
{
	FuncExpr *funcexpr;
	Oid		  funcoid;

	/* Look up bm25_get_current_score function */
	funcoid = LookupFuncName(
			list_make1(makeString("bm25_get_current_score")), 0, NULL, false);
	if (!OidIsValid(funcoid))
		return NULL;

	funcexpr				 = makeNode(FuncExpr);
	funcexpr->funcid		 = funcoid;
	funcexpr->funcresulttype = FLOAT8OID;
	funcexpr->funcretset	 = false;
	funcexpr->funcvariadic	 = false;
	funcexpr->funcformat	 = COERCE_EXPLICIT_CALL;
	funcexpr->funccollid	 = InvalidOid;
	funcexpr->inputcollid	 = InvalidOid;
	funcexpr->args			 = NIL;
	funcexpr->location		 = -1;

	return funcexpr;
}

/*
 * Replace BM25 score expressions in a plan node's targetlist.
 */
static void
replace_scores_in_targetlist(List *targetlist, BM25OidCache *oids)
{
	ListCell *lc;
	Expr	 *orderby_expr = NULL;

	/* First pass: find the resjunk ORDER BY expression */
	foreach (lc, targetlist)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(lc);

		if (tle->resjunk && is_bm25_score_opexpr((Node *)tle->expr, oids))
		{
			orderby_expr = tle->expr;
			break;
		}
	}

	if (orderby_expr == NULL)
		return;

	/* Second pass: replace the ORDER BY expr and any matching SELECT exprs */
	foreach (lc, targetlist)
	{
		TargetEntry *tle = (TargetEntry *)lfirst(lc);

		if (is_bm25_score_opexpr((Node *)tle->expr, oids) &&
			equal(tle->expr, orderby_expr))
		{
			FuncExpr *stub = make_stub_funcexpr();

			if (stub != NULL)
				tle->expr = (Expr *)stub;
		}
	}
}

/*
 * Check if the plan contains a BM25 IndexScan that will compute scores.
 */
static bool
plan_has_bm25_indexscan(Plan *plan, BM25OidCache *oids)
{
	ListCell *lc;

	if (plan == NULL)
		return false;

	/* Check if this is a BM25 IndexScan */
	if (IsA(plan, IndexScan))
	{
		IndexScan *indexscan = (IndexScan *)plan;
		Oid		   indexamid;
		HeapTuple  classTuple;

		/* Look up the index's access method */
		classTuple =
				SearchSysCache1(RELOID, ObjectIdGetDatum(indexscan->indexid));
		if (HeapTupleIsValid(classTuple))
		{
			Form_pg_class classForm = (Form_pg_class)GETSTRUCT(classTuple);

			indexamid = classForm->relam;
			ReleaseSysCache(classTuple);

			if (indexamid == oids->bm25_am_oid)
				return true;
		}
	}

	/* Recurse into child plans */
	if (plan_has_bm25_indexscan(plan->lefttree, oids))
		return true;
	if (plan_has_bm25_indexscan(plan->righttree, oids))
		return true;

	/* Handle plan-specific children */
	switch (nodeTag(plan))
	{
	case T_Append:
	{
		Append *append = (Append *)plan;

		foreach (lc, append->appendplans)
			if (plan_has_bm25_indexscan((Plan *)lfirst(lc), oids))
				return true;
	}
	break;

	case T_MergeAppend:
	{
		MergeAppend *merge = (MergeAppend *)plan;

		foreach (lc, merge->mergeplans)
			if (plan_has_bm25_indexscan((Plan *)lfirst(lc), oids))
				return true;
	}
	break;

	case T_SubqueryScan:
	{
		SubqueryScan *subquery = (SubqueryScan *)plan;

		if (plan_has_bm25_indexscan(subquery->subplan, oids))
			return true;
	}
	break;

	default:
		break;
	}

	return false;
}

/*
 * Recursively walk the plan tree and replace BM25 score expressions.
 */
static void
replace_scores_in_plan(Plan *plan, BM25OidCache *oids)
{
	if (plan == NULL)
		return;

	/* Process this node's targetlist */
	replace_scores_in_targetlist(plan->targetlist, oids);

	/* Recurse into child plans */
	replace_scores_in_plan(plan->lefttree, oids);
	replace_scores_in_plan(plan->righttree, oids);

	/* Handle plan-specific children */
	switch (nodeTag(plan))
	{
	case T_Append:
	{
		Append	 *append = (Append *)plan;
		ListCell *lc;

		foreach (lc, append->appendplans)
			replace_scores_in_plan((Plan *)lfirst(lc), oids);
	}
	break;

	case T_MergeAppend:
	{
		MergeAppend *merge = (MergeAppend *)plan;
		ListCell	*lc;

		foreach (lc, merge->mergeplans)
			replace_scores_in_plan((Plan *)lfirst(lc), oids);
	}
	break;

	case T_SubqueryScan:
	{
		SubqueryScan *subquery = (SubqueryScan *)plan;

		replace_scores_in_plan(subquery->subplan, oids);
	}
	break;

	default:
		break;
	}
}

/*
 * Planner hook: called after standard_planner produces the plan.
 */
static PlannedStmt *
tp_planner_hook(
		Query		 *parse,
		const char	 *query_string,
		int			  cursorOptions,
		ParamListInfo boundParams)
{
	PlannedStmt *result;
	BM25OidCache oid_cache;

	/* Call previous hook or standard planner */
	if (prev_planner_hook)
		result = prev_planner_hook(
				parse, query_string, cursorOptions, boundParams);
	else
		result = standard_planner(
				parse, query_string, cursorOptions, boundParams);

	/* Only process if we have our extension's OIDs and BM25 index scan */
	if (get_bm25_oids(&oid_cache) && result->planTree != NULL &&
		plan_has_bm25_indexscan(result->planTree, &oid_cache))
	{
		replace_scores_in_plan(result->planTree, &oid_cache);
	}

	return result;
}

/*
 * Initialize planner hooks (called from _PG_init).
 */
void
tp_planner_hook_init(void)
{
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook		 = tp_post_parse_analyze_hook;

	prev_planner_hook = planner_hook;
	planner_hook	  = tp_planner_hook;
}
