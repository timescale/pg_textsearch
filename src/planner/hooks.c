/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * hooks.c - Parse analysis and planner hooks
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
#include <catalog/pg_inherits.h>
#include <catalog/pg_opclass.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <catalog/pg_type_d.h>
#include <commands/defrem.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/plannodes.h>
#include <optimizer/optimizer.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
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
static set_rel_pathlist_hook_type	prev_set_rel_pathlist_hook	 = NULL;

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
 * Structure to track explicit index requirements for a relation/column.
 * Used during planning to force the planner to use the specified index.
 */
typedef struct ExplicitIndexRequirement
{
	Oid	 relid;				 /* Table OID */
	Oid	 required_index_oid; /* The index that must be used */
	bool is_explicit;		 /* true if from to_bm25query with index name */
} ExplicitIndexRequirement;

/*
 * Planning context - stores explicit index requirements during planning.
 * This is set before standard_planner and cleared after.
 */
typedef struct PlanningContext
{
	List *explicit_indexes; /* List of ExplicitIndexRequirement */
	Oid	  bm25_am_oid;		/* Cached BM25 access method OID */
} PlanningContext;

static PlanningContext *current_planning_context = NULL;

/*
 * Flag to track if the current query has any BM25 operators.
 * Set during post_parse_analyze, used in planner_hook to skip expensive
 * plan tree walks for non-BM25 queries.
 */
static bool query_has_bm25_operators = false;

/*
 * Context for query tree mutation
 */
typedef struct ResolveIndexContext
{
	Query		 *query;
	BM25OidCache *oid_cache;
	bool		  found_bm25_operator; /* Set to true if any BM25 op found */
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
 * Check if an index is on a specific column of a table.
 * Returns true if the index is on the given table and column.
 */
static bool
index_is_on_column(Oid index_oid, Oid relid, AttrNumber attnum)
{
	HeapTuple	  idx_tuple;
	Form_pg_index idx_form;
	bool		  result = false;

	idx_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(idx_tuple))
		return false;

	idx_form = (Form_pg_index)GETSTRUCT(idx_tuple);

	/* Check if index is on the correct table */
	if (idx_form->indrelid == relid)
	{
		/* Check if index is on the correct column */
		int nkeys = idx_form->indnatts;
		for (int i = 0; i < nkeys; i++)
		{
			if (idx_form->indkey.values[i] == attnum)
			{
				result = true;
				break;
			}
		}
	}

	ReleaseSysCache(idx_tuple);
	return result;
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
 * Also validates that explicit indexes are on the correct column.
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

	/* Mark that we found a BM25 operator for later optimization */
	context->found_bm25_operator = true;
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

	/*
	 * If the bm25query has an explicit index, validate that the index is on
	 * the column being queried. This catches cases where the user specifies
	 * an index that is on a different column (GitHub issue #194).
	 */
	if (OidIsValid(tpquery->index_oid))
	{
		if (IsA(left, Var))
		{
			Var		  *var = (Var *)left;
			Oid		   relid;
			AttrNumber attnum;

			if (get_var_relation_and_attnum(
						var, context->query, &relid, &attnum))
			{
				if (!index_is_on_column(tpquery->index_oid, relid, attnum))
				{
					char *index_name = get_rel_name(tpquery->index_oid);
					char *col_name	 = get_attname(relid, attnum, false);
					char *table_name = get_rel_name(relid);

					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("index \"%s\" is not on column \"%s\"",
									index_name ? index_name : "(unknown)",
									col_name ? col_name : "(unknown)"),
							 errdetail("The explicitly specified index is not "
									   "built on the column being searched."),
							 errhint("Use an index that is built on column "
									 "\"%s\" of table \"%s\", or omit the "
									 "index name to use automatic index "
									 "resolution.",
									 col_name ? col_name : "(unknown)",
									 table_name ? table_name : "(unknown)")));
				}
			}
		}
		return NULL; /* Already resolved, validation passed */
	}

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

	/* Mark that we found a BM25 operator for later optimization */
	context->found_bm25_operator = true;
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

	context.query				= query;
	context.oid_cache			= &oid_cache;
	context.found_bm25_operator = false;

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

	/*
	 * Track if this query has BM25 operators for the planner hook.
	 * This avoids expensive plan tree walks for non-BM25 queries.
	 */
	if (context.found_bm25_operator)
		query_has_bm25_operators = true;
}

/*
 * Post parse analysis hook function.
 *
 * Performance note: For non-BM25 queries, this does minimal work - just a
 * cached OID lookup and a quick walk of expressions to check operator OIDs.
 * Expensive operations (syscache lookups, index resolution) only happen
 * for actual BM25 operator expressions.
 */
static void
tp_post_parse_analyze_hook(
		ParseState *pstate	pg_attribute_unused(),
		Query			   *query,
		JumbleState *jstate pg_attribute_unused())
{
	/* Reset flag for this query - will be set if BM25 operators found */
	query_has_bm25_operators = false;

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

	case T_CustomScan:
	{
		CustomScan *cscan = (CustomScan *)plan;

		foreach (lc, cscan->custom_plans)
			if (plan_has_bm25_indexscan((Plan *)lfirst(lc), oids))
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

	case T_CustomScan:
	{
		CustomScan *cscan = (CustomScan *)plan;
		ListCell   *lc2;

		foreach (lc2, cscan->custom_plans)
			replace_scores_in_plan((Plan *)lfirst(lc2), oids);
	}
	break;

	default:
		break;
	}
}

/*
 * Extract a TpQuery from an expression, if present.
 * Returns NULL if the expression doesn't contain a bm25query constant.
 */
static TpQuery *
extract_tpquery_from_expr(Node *node, BM25OidCache *oids)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, OpExpr))
	{
		OpExpr *opexpr = (OpExpr *)node;

		if (opexpr->opno == oids->text_tpquery_operator_oid &&
			list_length(opexpr->args) == 2)
		{
			Node *right = lsecond(opexpr->args);

			if (IsA(right, Const))
			{
				Const *constNode = (Const *)right;

				if (constNode->consttype == oids->tpquery_type_oid &&
					!constNode->constisnull)
				{
					return (TpQuery *)DatumGetPointer(constNode->constvalue);
				}
			}
		}
	}

	return NULL;
}

/*
 * Find a TpQuery in a list of expressions.
 * Returns NULL if no bm25query constant found.
 */
static TpQuery *
find_tpquery_in_list(List *exprlist, BM25OidCache *oids)
{
	ListCell *lc;

	foreach (lc, exprlist)
	{
		Node	*node	 = (Node *)lfirst(lc);
		TpQuery *tpquery = extract_tpquery_from_expr(node, oids);

		if (tpquery != NULL)
			return tpquery;
	}

	return NULL;
}

/*
 * Check if scan_index is a descendant of specified_index (for partitioned
 * tables). Returns true if specified_index is a partitioned index and
 * scan_index is one of its descendants (child, grandchild, etc.).
 *
 * This walks up the inheritance chain from scan_index to see if it eventually
 * reaches specified_index.
 */
static bool
is_child_partition_index(Oid specified_index_oid, Oid scan_index_oid)
{
	char relkind;
	Oid	 current_oid;
	int	 depth;

	/* Check if specified index is a partitioned index */
	relkind = get_rel_relkind(specified_index_oid);
	if (relkind != RELKIND_PARTITIONED_INDEX)
		return false;

	/*
	 * Walk up the inheritance chain from scan_index_oid. At each step, look
	 * up the parent in pg_inherits. If we reach specified_index_oid, return
	 * true. Limit depth to prevent infinite loops.
	 */
	current_oid = scan_index_oid;
	for (depth = 0; depth < 100; depth++)
	{
		Relation	inhrel;
		SysScanDesc scan;
		HeapTuple	tuple;
		ScanKeyData skey;
		Oid			parent_oid = InvalidOid;

		inhrel = table_open(InheritsRelationId, AccessShareLock);

		ScanKeyInit(
				&skey,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(current_oid));

		scan = systable_beginscan(
				inhrel, InheritsRelidSeqnoIndexId, true, NULL, 1, &skey);

		tuple = systable_getnext(scan);
		if (tuple != NULL)
		{
			Form_pg_inherits inhform = (Form_pg_inherits)GETSTRUCT(tuple);
			parent_oid				 = inhform->inhparent;
		}

		systable_endscan(scan);
		table_close(inhrel, AccessShareLock);

		if (!OidIsValid(parent_oid))
			return false; /* No parent, reached top without finding match */

		if (parent_oid == specified_index_oid)
			return true; /* Found the specified index in the chain */

		current_oid = parent_oid;
	}

	return false; /* Depth limit exceeded */
}

/*
 * Get the table OID that an index is built on.
 */
static Oid
get_index_table_oid(Oid index_oid)
{
	HeapTuple	  idx_tuple;
	Form_pg_index idx_form;
	Oid			  result = InvalidOid;

	idx_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (HeapTupleIsValid(idx_tuple))
	{
		idx_form = (Form_pg_index)GETSTRUCT(idx_tuple);
		result	 = idx_form->indrelid;
		ReleaseSysCache(idx_tuple);
	}
	return result;
}

/*
 * Check if an index is a BM25 index.
 */
static bool
is_bm25_index(Oid index_oid, Oid bm25_am_oid)
{
	HeapTuple classTuple;
	bool	  result = false;

	classTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(index_oid));
	if (HeapTupleIsValid(classTuple))
	{
		Form_pg_class classForm = (Form_pg_class)GETSTRUCT(classTuple);
		result					= (classForm->relam == bm25_am_oid);
		ReleaseSysCache(classTuple);
	}
	return result;
}

/*
 * Walker context for collecting explicit index requirements from a query.
 */
typedef struct CollectExplicitIndexContext
{
	BM25OidCache *oid_cache;
	List		 *requirements; /* List of ExplicitIndexRequirement */
} CollectExplicitIndexContext;

/*
 * Walker to find explicit index requirements in query expressions.
 */
static bool
collect_explicit_indexes_walker(
		Node *node, CollectExplicitIndexContext *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, OpExpr))
	{
		OpExpr *opexpr = (OpExpr *)node;

		if (opexpr->opno == context->oid_cache->text_tpquery_operator_oid &&
			list_length(opexpr->args) == 2)
		{
			Node *right = lsecond(opexpr->args);

			/*
			 * Try to fold FuncExpr (e.g., to_bm25query()) to a Const.
			 * This is needed because parse analysis may not have folded
			 * the function call if the index was already explicit.
			 */
			if (IsA(right, FuncExpr))
				right = eval_const_expressions(NULL, right);

			if (IsA(right, Const))
			{
				Const *constNode = (Const *)right;

				if (constNode->consttype ==
							context->oid_cache->tpquery_type_oid &&
					!constNode->constisnull)
				{
					TpQuery *tpquery = (TpQuery *)DatumGetPointer(
							constNode->constvalue);
					Oid index_oid = get_tpquery_index_oid(tpquery);

					if (OidIsValid(index_oid))
					{
						ExplicitIndexRequirement *req;
						Oid						  table_oid;

						table_oid = get_index_table_oid(index_oid);
						if (OidIsValid(table_oid))
						{
							req = palloc(sizeof(ExplicitIndexRequirement));
							req->relid				= table_oid;
							req->required_index_oid = index_oid;
							req->is_explicit = tpquery_is_explicit_index(
									tpquery);
							context->requirements =
									lappend(context->requirements, req);
						}
					}
				}
			}
		}
	}

	return expression_tree_walker(
			node, collect_explicit_indexes_walker, context);
}

/*
 * Collect all explicit index requirements from a query.
 *
 * Performance note: This walks the query tree, which is O(n) where n is the
 * number of nodes. However, the per-node work is minimal - just a type check
 * and integer comparison for non-BM25 queries. The expensive work (folding
 * FuncExpr, syscache lookups) only happens for actual BM25 operator
 * expressions with explicit index names, which is rare.
 */
static List *
collect_explicit_index_requirements(Query *parse, BM25OidCache *oid_cache)
{
	CollectExplicitIndexContext context;

	context.oid_cache	 = oid_cache;
	context.requirements = NIL;

	/* Walk the entire query tree */
	query_tree_walker(parse, collect_explicit_indexes_walker, &context, 0);

	return context.requirements;
}

/*
 * Find an explicit index requirement for a given relation.
 * Returns NULL if no explicit requirement exists.
 */
static ExplicitIndexRequirement *
find_explicit_requirement_for_rel(Oid relid)
{
	ListCell *lc;

	if (current_planning_context == NULL)
		return NULL;

	foreach (lc, current_planning_context->explicit_indexes)
	{
		ExplicitIndexRequirement *req = (ExplicitIndexRequirement *)lfirst(lc);

		if (req->relid == relid)
			return req;
	}

	return NULL;
}

/*
 * Check if a path uses a specific BM25 index.
 * Returns the index OID if it's a BM25 IndexPath, InvalidOid otherwise.
 */
static Oid
get_path_bm25_index_oid(Path *path, Oid bm25_am_oid)
{
	if (IsA(path, IndexPath))
	{
		IndexPath *indexpath = (IndexPath *)path;

		if (is_bm25_index(indexpath->indexinfo->indexoid, bm25_am_oid))
			return indexpath->indexinfo->indexoid;
	}
	return InvalidOid;
}

/*
 * Find the IndexOptInfo for a specific index in the relation's indexlist.
 */
static IndexOptInfo *
find_index_opt_info(RelOptInfo *rel, Oid index_oid)
{
	ListCell *lc;

	foreach (lc, rel->indexlist)
	{
		IndexOptInfo *indexinfo = (IndexOptInfo *)lfirst(lc);

		if (indexinfo->indexoid == index_oid)
			return indexinfo;
	}

	return NULL;
}

/*
 * set_rel_pathlist_hook: Replace BM25 IndexPaths that use the wrong index.
 *
 * When a query specifies an explicit BM25 index via to_bm25query(), we need
 * to ensure the planner uses that index. This hook finds BM25 IndexPaths
 * using the wrong index and updates them to use the correct one.
 *
 * For partitioned tables, we allow child partition indexes of the specified
 * parent index.
 *
 * Performance note: This hook returns immediately for non-BM25 queries since
 * current_planning_context will be NULL (we only set it when there are
 * explicit index requirements). The overhead for non-BM25 queries is just
 * a single pointer comparison.
 */
static void
tp_set_rel_pathlist_hook(
		PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	ExplicitIndexRequirement *req;
	IndexOptInfo			 *correct_indexinfo;
	ListCell				 *lc;

	/* Call previous hook first */
	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	/* Only process base relations */
	if (rel->reloptkind != RELOPT_BASEREL)
		return;

	/* Check if we have planning context */
	if (current_planning_context == NULL)
		return;

	/* Find explicit requirement for this relation */
	req = find_explicit_requirement_for_rel(rte->relid);
	if (req == NULL || !req->is_explicit)
		return;

	/* Find the IndexOptInfo for the correct index */
	correct_indexinfo = find_index_opt_info(rel, req->required_index_oid);
	if (correct_indexinfo == NULL)
	{
		/* Required index not found in relation's indexlist - can't fix */
		return;
	}

	/*
	 * Find BM25 IndexPaths using the wrong index and update them to use
	 * the correct index. Since both indexes are on the same column with
	 * compatible operators, we can safely swap the indexinfo pointer.
	 */
	foreach (lc, rel->pathlist)
	{
		Path *path = (Path *)lfirst(lc);
		Oid	  index_oid;

		if (!IsA(path, IndexPath))
			continue;

		index_oid = get_path_bm25_index_oid(
				path, current_planning_context->bm25_am_oid);

		if (!OidIsValid(index_oid))
			continue; /* Not a BM25 index path */

		if (index_oid == req->required_index_oid)
			continue; /* Already using correct index */

		if (is_child_partition_index(req->required_index_oid, index_oid))
			continue; /* Child partition of required index */

		/* Swap to the correct index */
		((IndexPath *)path)->indexinfo = correct_indexinfo;
	}

	/* Same for partial paths */
	foreach (lc, rel->partial_pathlist)
	{
		Path *path = (Path *)lfirst(lc);
		Oid	  index_oid;

		if (!IsA(path, IndexPath))
			continue;

		index_oid = get_path_bm25_index_oid(
				path, current_planning_context->bm25_am_oid);

		if (!OidIsValid(index_oid))
			continue;

		if (index_oid == req->required_index_oid)
			continue;

		if (is_child_partition_index(req->required_index_oid, index_oid))
			continue;

		((IndexPath *)path)->indexinfo = correct_indexinfo;
	}
}

/*
 * Check an IndexScan node for explicit index mismatch.
 * If the IndexScan uses a different index than what's specified in the
 * bm25query, raise an error (if explicit) or warning (if implicit).
 *
 * Exception: partitioned indexes - if the specified index is a partitioned
 * index and the scan uses a child partition index, that's allowed.
 */
static void
validate_indexscan_explicit_index(IndexScan *indexscan, BM25OidCache *oids)
{
	TpQuery	 *tpquery;
	Oid		  specified_index_oid;
	HeapTuple classTuple;
	Oid		  indexamid;

	/* First check this is a BM25 index scan */
	classTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(indexscan->indexid));
	if (!HeapTupleIsValid(classTuple))
		return;

	indexamid = ((Form_pg_class)GETSTRUCT(classTuple))->relam;
	ReleaseSysCache(classTuple);

	if (indexamid != oids->bm25_am_oid)
		return; /* Not a BM25 index scan */

	/* Look for bm25query in the indexorderby expressions */
	tpquery = find_tpquery_in_list(indexscan->indexorderby, oids);
	if (tpquery == NULL)
	{
		/* Also check the indexqual */
		tpquery = find_tpquery_in_list(indexscan->indexqual, oids);
	}

	if (tpquery == NULL)
		return; /* No bm25query found */

	specified_index_oid = get_tpquery_index_oid(tpquery);
	if (!OidIsValid(specified_index_oid))
		return; /* No index specified in query */

	/* Validate they match */
	if (specified_index_oid != indexscan->indexid)
	{
		char *specified_name;
		char *scan_name;

		/*
		 * Allow partitioned indexes: if specified is parent and scan is child
		 */
		if (is_child_partition_index(specified_index_oid, indexscan->indexid))
			return; /* Child partition index - allowed */

		specified_name = get_rel_name(specified_index_oid);
		scan_name	   = get_rel_name(indexscan->indexid);

		/*
		 * Check if the index was explicitly specified by the user via
		 * to_bm25query(text, index_name). If so, error. If it was implicitly
		 * resolved (e.g., from text <@> text syntax), just warn.
		 */
		if (tpquery_is_explicit_index(tpquery))
		{
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("query specifies index \"%s\" but planner chose "
							"index \"%s\"",
							specified_name ? specified_name : "(unknown)",
							scan_name ? scan_name : "(unknown)"),
					 errdetail("When an explicit index is specified in "
							   "to_bm25query(), that index must be used for "
							   "the scan to ensure consistent tokenization."),
					 errhint("Use a planner hint to force the specified "
							 "index, or remove the explicit index name to let "
							 "the planner choose automatically.")));
		}
		else
		{
			ereport(WARNING,
					(errmsg("planner chose index \"%s\" instead of \"%s\"",
							scan_name ? scan_name : "(unknown)",
							specified_name ? specified_name : "(unknown)"),
					 errhint("If this is not desired, use a planner hint to "
							 "force a specific index, or use explicit "
							 "to_bm25query('query', 'index_name').")));
		}
	}
}

/*
 * Validate that when an explicit index is specified in bm25query, the planner
 * uses that same index for the scan.
 *
 * Walks the plan tree looking for BM25 IndexScan nodes and validates their
 * ORDER BY expressions.
 */
static void
validate_explicit_index_usage(Plan *plan, BM25OidCache *oids)
{
	ListCell *lc;

	if (plan == NULL)
		return;

	/* Check this node if it's an IndexScan */
	if (IsA(plan, IndexScan))
	{
		validate_indexscan_explicit_index((IndexScan *)plan, oids);
	}

	/* Recurse into child plans */
	validate_explicit_index_usage(plan->lefttree, oids);
	validate_explicit_index_usage(plan->righttree, oids);

	/* Handle plan-specific children */
	switch (nodeTag(plan))
	{
	case T_Append:
	{
		Append *append = (Append *)plan;

		foreach (lc, append->appendplans)
			validate_explicit_index_usage((Plan *)lfirst(lc), oids);
	}
	break;

	case T_MergeAppend:
	{
		MergeAppend *merge = (MergeAppend *)plan;

		foreach (lc, merge->mergeplans)
			validate_explicit_index_usage((Plan *)lfirst(lc), oids);
	}
	break;

	case T_SubqueryScan:
	{
		SubqueryScan *subquery = (SubqueryScan *)plan;

		validate_explicit_index_usage(subquery->subplan, oids);
	}
	break;

	case T_CustomScan:
	{
		CustomScan *cscan = (CustomScan *)plan;

		foreach (lc, cscan->custom_plans)
			validate_explicit_index_usage((Plan *)lfirst(lc), oids);
	}
	break;

	default:
		break;
	}
}

/*
 * Planner hook: sets up planning context and post-processes the plan.
 *
 * We set up the planning context BEFORE calling standard_planner so that
 * set_rel_pathlist_hook can filter out unwanted BM25 index paths.
 */
static PlannedStmt *
tp_planner_hook(
		Query		 *parse,
		const char	 *query_string,
		int			  cursorOptions,
		ParamListInfo boundParams)
{
	PlannedStmt		*result;
	BM25OidCache	 oid_cache;
	PlanningContext	 planning_context;
	PlanningContext *saved_context;
	List			*explicit_indexes;

	/* Get BM25 OIDs - if extension not installed, just pass through */
	if (!get_bm25_oids(&oid_cache))
	{
		if (prev_planner_hook)
			return prev_planner_hook(
					parse, query_string, cursorOptions, boundParams);
		else
			return standard_planner(
					parse, query_string, cursorOptions, boundParams);
	}

	/*
	 * Collect explicit index requirements from the query. This walks the
	 * query tree looking for to_bm25query() calls with explicit index names.
	 *
	 * Performance optimization: Only set up the planning context if we
	 * actually find explicit requirements. For non-BM25 queries or queries
	 * without explicit index names, this avoids any overhead in the
	 * set_rel_pathlist_hook.
	 */
	explicit_indexes = collect_explicit_index_requirements(parse, &oid_cache);

	if (explicit_indexes != NIL)
	{
		planning_context.explicit_indexes = explicit_indexes;
		planning_context.bm25_am_oid	  = oid_cache.bm25_am_oid;

		/* Save and set current planning context */
		saved_context			 = current_planning_context;
		current_planning_context = &planning_context;
	}
	else
	{
		saved_context = NULL;
	}

	/* Call previous hook or standard planner */
	PG_TRY();
	{
		if (prev_planner_hook)
			result = prev_planner_hook(
					parse, query_string, cursorOptions, boundParams);
		else
			result = standard_planner(
					parse, query_string, cursorOptions, boundParams);
	}
	PG_FINALLY();
	{
		/* Restore previous context and clean up if we set one up */
		if (explicit_indexes != NIL)
		{
			current_planning_context = saved_context;
			list_free_deep(explicit_indexes);
		}
	}
	PG_END_TRY();

	/*
	 * Post-process the plan if it may have BM25 index scans.
	 *
	 * Performance optimization: Only check for BM25 IndexScans if the
	 * post_parse_analyze hook found BM25 operators. This avoids expensive
	 * syscache lookups in plan_has_bm25_indexscan() for non-BM25 queries.
	 */
	if (query_has_bm25_operators && result->planTree != NULL &&
		plan_has_bm25_indexscan(result->planTree, &oid_cache))
	{
		/*
		 * Validate implicit index resolution - for explicit indexes, the
		 * set_rel_pathlist_hook should have already forced the correct index.
		 * This validation catches cases where implicit resolution picked a
		 * different index than the planner chose.
		 */
		validate_explicit_index_usage(result->planTree, &oid_cache);

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

	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook	   = tp_set_rel_pathlist_hook;

	prev_planner_hook = planner_hook;
	planner_hook	  = tp_planner_hook;
}
