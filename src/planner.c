/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */

/*
 * planner.c - Parse analysis hook for implicit BM25 index resolution
 *
 * When a query uses the <@> operator with an unresolved tpquery (index_oid
 * is InvalidOid), this hook identifies the column being scored and finds
 * a suitable BM25 index to use.
 *
 * We use post_parse_analyze_hook rather than planner_hook because:
 * 1. The transformation must happen BEFORE path generation in the planner
 * 2. post_parse_analyze_hook runs right after parsing, before planning
 * 3. This allows the planner to see the transformed expressions and use
 *    index scans for ORDER BY with the <@> operator
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
#include <catalog/pg_type.h>
#include <catalog/pg_type_d.h>
#include <commands/defrem.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <optimizer/optimizer.h>
#include <parser/analyze.h>
#include <parser/parse_oper.h>
#include <parser/parsetree.h>
#include <utils/builtins.h>
#include <utils/catcache.h>
#include <utils/fmgroids.h>
#include <utils/inval.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/syscache.h>

#include "operator.h"
#include "planner.h"
#include "query.h"

/* Previous post_parse_analyze hook in chain */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;

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

/*
 * Initialize planner hook (called from _PG_init).
 */
void
tp_planner_hook_init(void)
{
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook		 = tp_post_parse_analyze_hook;
}
