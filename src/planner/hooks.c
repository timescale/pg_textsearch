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

#include <access/amapi.h>
#include <access/genam.h>
#include <access/htup_details.h>
#include <access/sysattr.h>
#include <access/table.h>
#include <access/transam.h>
#include <access/xact.h>
#include <catalog/indexing.h>
#include <catalog/namespace.h>
#include <catalog/partition.h>
#include <catalog/pg_am.h>
#include <catalog/pg_extension.h>
#include <catalog/pg_index.h>
#include <catalog/pg_inherits.h>
#include <catalog/pg_opclass.h>
#include <catalog/pg_operator.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <catalog/pg_type_d.h>
#include <commands/defrem.h>
#include <commands/extension.h>
#include <limits.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/plannodes.h>
#include <optimizer/optimizer.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <optimizer/plancat.h>
#include <optimizer/planner.h>
#include <parser/analyze.h>
#include <parser/parse_func.h>
#include <parser/parse_oper.h>
#include <parser/parsetree.h>
#include <rewrite/rewriteManip.h>
#include <utils/builtins.h>
#include <utils/catcache.h>
#include <utils/fmgroids.h>
#include <utils/hsearch.h>
#include <utils/inval.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/syscache.h>

#include "compat.h"
#include "index/limit.h"
#include "planner/hooks.h"
#include "scoring/bm25.h"
#include "types/query.h"

/* Previous hooks in chain */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static planner_hook_type			prev_planner_hook			 = NULL;
static set_rel_pathlist_hook_type	prev_set_rel_pathlist_hook	 = NULL;
#if PG_VERSION_NUM >= 190000
static build_simple_rel_hook_type prev_build_simple_rel_hook = NULL;
#else
static get_relation_info_hook_type prev_get_relation_info_hook = NULL;
#endif

/*
 * Structure to hold looked-up OIDs for a query
 */
typedef struct BM25OidCache
{
	Oid bm25_am_oid;
	Oid tpquery_type_oid;
	Oid text_tpquery_operator_oid;
	Oid text_text_operator_oid;
	Oid textarray_tpquery_operator_oid;
	Oid textarray_text_operator_oid;
	Oid score_stub_func_oid; /* bm25_get_current_score() */
	Oid partition_score_func_oid;
	Oid partition_array_score_func_oid;
} BM25OidCache;

/* Set while planning a query whose scores may need a row's partition. */
static BM25OidCache *partition_carry_oids = NULL;

/*
 * Backend-local cache to avoid repeated syscache lookups.
 * Cache is invalidated when the extension or its AM, type, or operators
 * change.
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
	Oid		   relid;			   /* Table OID */
	Index	   rti;				   /* Range table entry at this query level */
	AttrNumber attnum;			   /* Column attr (0 for expr) */
	Node	  *expr;			   /* Expression (NULL for cols) */
	Oid		   required_index_oid; /* The index that must be used */
} ExplicitIndexRequirement;

typedef struct ExplicitIndexMatch
{
	RelOptInfo	 *rel;
	Oid			  required_index_oid;
	IndexOptInfo *indexinfo;
} ExplicitIndexMatch;

/*
 * One cache entry per query planned with its own PlannerInfo, including
 * subqueries not pulled up into their parent. Sibling subqueries have
 * separate entries even at the same nesting depth. Each entry holds that
 * query's list of ExplicitIndexRequirement objects.
 */
typedef struct ExplicitIndexRequirements
{
	PlannerInfo *root;		   /* Query planner; hash key must be first */
	List		*requirements; /* ExplicitIndexRequirement list */
} ExplicitIndexRequirements;

/*
 * Cache for one planner invocation, shared by the queries it plans.
 * Recursive planner invocations save and restore the outer context.
 */
typedef struct PlanningContext
{
	BM25OidCache			 *oid_cache;	 /* Borrowed planner OIDs */
	MemoryContext			  cache_context; /* Owns this invocation's cache */
	ExplicitIndexRequirements first; /* First query; NULL root if unused */
	HTAB *other_roots;				 /* Other queries' requirement lists */
	List *index_matches;			 /* List of ExplicitIndexMatch */
} PlanningContext;

static PlanningContext *current_planning_context = NULL;

/*
 * Context for query tree mutation
 */
typedef struct ResolveIndexContext
{
	Query		 *query;
	BM25OidCache *oid_cache;
} ResolveIndexContext;

/*
 * Syscache invalidation callback for the catalogs used by the OID lookup.
 */
static void
bm25_cache_invalidation_callback(
		Datum arg			   pg_attribute_unused(),
		TP_SYSCACHE_ID cacheid pg_attribute_unused(),
		uint32 hashvalue	   pg_attribute_unused())
{
	oid_cache_state = CACHE_UNKNOWN;
}

/*
 * Look up OIDs for BM25 extension objects (uncached).
 * Returns false if extension is not installed.
 *
 * Use qualified lookups for bm25query type and
 * <@> operators to prevent search-path-based type/operator squatting.
 */
static bool
lookup_bm25_oids_internal(BM25OidCache *cache)
{
	HeapTuple tuple;
	Oid		  extension_oid;
	Oid		  schema_oid;

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

	/* Resolve pg_textsearch schema OID for qualified lookups */
	extension_oid = get_extension_oid("pg_textsearch", true);
	if (!OidIsValid(extension_oid))
		return false;
	schema_oid = get_extension_schema(extension_oid);
	if (!OidIsValid(schema_oid))
		return false;

	/* Look up bm25query type using qualified schema OID (not search_path) */
	cache->tpquery_type_oid = GetSysCacheOid2(
			TYPENAMENSP,
			Anum_pg_type_oid,
			CStringGetDatum("bm25query"),
			ObjectIdGetDatum(schema_oid));
	if (!OidIsValid(cache->tpquery_type_oid))
		return false;

	/* Resolve all four signatures in the extension namespace, never
	 * search_path. */
	cache->text_tpquery_operator_oid = GetSysCacheOid4(
			OPERNAMENSP,
			Anum_pg_operator_oid,
			CStringGetDatum("<@>"),
			ObjectIdGetDatum(TEXTOID),
			ObjectIdGetDatum(cache->tpquery_type_oid),
			ObjectIdGetDatum(schema_oid));
	cache->text_text_operator_oid = GetSysCacheOid4(
			OPERNAMENSP,
			Anum_pg_operator_oid,
			CStringGetDatum("<@>"),
			ObjectIdGetDatum(TEXTOID),
			ObjectIdGetDatum(TEXTOID),
			ObjectIdGetDatum(schema_oid));
	cache->textarray_tpquery_operator_oid = GetSysCacheOid4(
			OPERNAMENSP,
			Anum_pg_operator_oid,
			CStringGetDatum("<@>"),
			ObjectIdGetDatum(TEXTARRAYOID),
			ObjectIdGetDatum(cache->tpquery_type_oid),
			ObjectIdGetDatum(schema_oid));
	cache->textarray_text_operator_oid = GetSysCacheOid4(
			OPERNAMENSP,
			Anum_pg_operator_oid,
			CStringGetDatum("<@>"),
			ObjectIdGetDatum(TEXTARRAYOID),
			ObjectIdGetDatum(TEXTOID),
			ObjectIdGetDatum(schema_oid));

	/*
	 * Resolve the zero-argument score stub in the extension namespace by OID.
	 * The planner injects this call into plans on the user's behalf, so it
	 * must neither follow the user's search_path (a role with CREATE on an
	 * earlier schema could supply its own function) nor require the user to
	 * hold USAGE on the extension schema (a name-based lookup would).
	 */
	cache->score_stub_func_oid = GetSysCacheOid3(
			PROCNAMEARGSNSP,
			Anum_pg_proc_oid,
			CStringGetDatum("bm25_get_current_score"),
			PointerGetDatum(buildoidvector(NULL, 0)),
			ObjectIdGetDatum(schema_oid));

	{
		Oid		   args[]	= {TEXTOID, cache->tpquery_type_oid, INTERNALOID};
		oidvector *argtypes = buildoidvector(args, 3);

		cache->partition_score_func_oid = GetSysCacheOid3(
				PROCNAMEARGSNSP,
				Anum_pg_proc_oid,
				CStringGetDatum("bm25_text_bm25query_score"),
				PointerGetDatum(argtypes),
				ObjectIdGetDatum(schema_oid));
		args[0] = TEXTARRAYOID;
		pfree(argtypes);
		argtypes							  = buildoidvector(args, 3);
		cache->partition_array_score_func_oid = GetSysCacheOid3(
				PROCNAMEARGSNSP,
				Anum_pg_proc_oid,
				CStringGetDatum("bm25_textarray_bm25query_score"),
				PointerGetDatum(argtypes),
				ObjectIdGetDatum(schema_oid));
		pfree(argtypes);
	}

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
		CacheRegisterSyscacheCallback(
				OPEROID, bm25_cache_invalidation_callback, (Datum)0);
		CacheRegisterSyscacheCallback(
				PROCOID, bm25_cache_invalidation_callback, (Datum)0);
		/*
		 * Older PG17 releases have no extension syscache.  Their AM/type/
		 * operator invalidations also cover extension create/drop; this
		 * extension is not relocatable.  Do not use the PG17 backpatch:
		 * binaries built with newer headers may run on older minors
		 * where its cache ID does not exist.
		 */
#if PG_VERSION_NUM >= 180000
		CacheRegisterSyscacheCallback(
				EXTENSIONOID, bm25_cache_invalidation_callback, (Datum)0);
#endif
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
 * Whether the extension's SQL defines the overloads that carry a row's
 * partition.  A new binary lacks them until ALTER EXTENSION ... UPDATE.
 */
bool
tp_partition_scoring_available(void)
{
	BM25OidCache oids;

	return get_bm25_oids(&oids) && OidIsValid(oids.partition_score_func_oid) &&
		   OidIsValid(oids.partition_array_score_func_oid);
}

/*
 * Check if an index is on a specific column of a table.
 * The index may belong to a descendant partition of the queried table.
 */
static bool
index_is_on_column(Oid index_oid, Oid relid, AttrNumber attnum)
{
	HeapTuple	  idx_tuple;
	Form_pg_index idx_form;
	bool		  matches_relation;
	bool		  result = false;

	idx_tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
	if (!HeapTupleIsValid(idx_tuple))
		return false;

	idx_form = (Form_pg_index)GETSTRUCT(idx_tuple);

	matches_relation = idx_form->indrelid == relid;
	if (!matches_relation && get_rel_relispartition(idx_form->indrelid))
	{
		List *ancestors = get_partition_ancestors(idx_form->indrelid);

		matches_relation = list_member_oid(ancestors, relid);
		list_free(ancestors);

		if (matches_relation)
		{
			char *column_name = get_attname(relid, attnum, false);

			/* Attached partitions can have different attribute numbers. */
			attnum = get_attnum(idx_form->indrelid, column_name);
			pfree(column_name);
		}
	}

	/* Check if index is on the correct table */
	if (matches_relation && attnum != InvalidAttrNumber)
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
	Oid			result			= InvalidOid;
	int			index_count		= 0;
	int			partial_skipped = 0;
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
			bool  predNull;
			Datum predDatum;
			int	  nkeys = indexForm->indnatts;

			/*
			 * Skip partial indexes for implicit
			 * resolution.  Users must name them
			 * explicitly via to_bm25query.
			 */
			predDatum = SysCacheGetAttr(
					INDEXRELID, indexTuple, Anum_pg_index_indpred, &predNull);
			(void)predDatum;
			if (!predNull)
			{
				for (int i = 0; i < nkeys; i++)
				{
					if (indexForm->indkey.values[i] == attnum)
					{
						partial_skipped++;
						break;
					}
				}
				ReleaseSysCache(classTuple);
				continue;
			}

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

	if (index_count == 0 && partial_skipped > 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no non-partial BM25 index found "
						"for this column "
						"(%d partial index(es) skipped)",
						partial_skipped),
				 errhint("Use to_bm25query('query', "
						 "'index_name') to specify which "
						 "partial index to use.")));

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
 * Extract relation OID from an arbitrary expression.
 * Returns InvalidOid if vars reference multiple relations
 * or none. Sets *varno_out to the varno of the relation.
 */
static Oid
get_expr_relid(Node *expr, Query *query, Index *varno_out)
{
	List	 *vars;
	ListCell *lc;
	Oid		  relid = InvalidOid;
	Index	  varno = 0;

	vars = pull_var_clause(expr, 0);
	if (vars == NIL)
		return InvalidOid;

	foreach (lc, vars)
	{
		Var			  *var = (Var *)lfirst(lc);
		RangeTblEntry *rte;

		if (var->varlevelsup != 0)
		{
			list_free(vars);
			return InvalidOid;
		}

		if (var->varno < 1 || var->varno > list_length(query->rtable))
		{
			list_free(vars);
			return InvalidOid;
		}

		rte = rt_fetch(var->varno, query->rtable);
		if (rte->rtekind != RTE_RELATION)
		{
			list_free(vars);
			return InvalidOid;
		}

		if (relid == InvalidOid)
		{
			relid = rte->relid;
			varno = var->varno;
		}
		else if (rte->relid != relid)
		{
			list_free(vars);
			return InvalidOid;
		}
	}

	list_free(vars);
	if (varno_out)
		*varno_out = varno;
	return relid;
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
 * Find BM25 index matching an expression on a relation.
 * For plain Var, delegates to find_bm25_index_for_column.
 * For complex expressions, compares expression trees.
 */
static Oid
find_bm25_index_for_expr(Node *expr, Oid relid, Index varno, Oid bm25_am_oid)
{
	Relation	indexRelation;
	SysScanDesc scan;
	HeapTuple	indexTuple;
	Oid			result			= InvalidOid;
	int			index_count		= 0;
	int			partial_skipped = 0;
	ScanKeyData scanKey;

	if (!OidIsValid(bm25_am_oid))
		return InvalidOid;

	/* Fast path: plain column reference */
	if (IsA(expr, Var))
	{
		Var *var = (Var *)expr;

		return find_bm25_index_for_column(relid, var->varattno, bm25_am_oid);
	}

	/* Slow path: expression comparison */
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
		Oid			  indexAmOid;

		classTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(indexOid));
		if (!HeapTupleIsValid(classTuple))
			continue;
		indexAmOid = ((Form_pg_class)GETSTRUCT(classTuple))->relam;
		ReleaseSysCache(classTuple);

		if (indexAmOid != bm25_am_oid)
			continue;

		/* Must be an expression index */
		if (indexForm->indkey.values[0] != 0)
			continue;

		/*
		 * Skip partial indexes for implicit resolution.
		 * Users must name them explicitly via
		 * to_bm25query.
		 */
		{
			Datum predDatum;
			bool  predNull;

			predDatum = SysCacheGetAttr(
					INDEXRELID, indexTuple, Anum_pg_index_indpred, &predNull);
			(void)predDatum;
			if (!predNull)
			{
				partial_skipped++;
				continue;
			}
		}

		{
			Datum exprDatum;
			bool  isnull;
			List *idx_exprs;
			Node *idx_expr;

			exprDatum = SysCacheGetAttr(
					INDEXRELID, indexTuple, Anum_pg_index_indexprs, &isnull);
			if (isnull)
				continue;

			idx_exprs = (List *)stringToNode(TextDatumGetCString(exprDatum));
			if (idx_exprs == NIL)
				continue;

			idx_expr = (Node *)linitial(idx_exprs);

			/*
			 * pg_index stores expressions with varno=1.
			 * Normalize to match the query's varno.
			 */
			ChangeVarNodes(idx_expr, 1, varno, 0);

			if (equal(idx_expr, expr))
			{
				index_count++;
				if (result == InvalidOid)
					result = indexOid;
			}
		}
	}

	systable_endscan(scan);
	table_close(indexRelation, AccessShareLock);

	if (index_count > 1)
		ereport(WARNING,
				(errmsg("multiple BM25 indexes match the "
						"expression"),
				 errhint("Use explicit to_bm25query('query',"
						 " 'index_name') to specify which "
						 "index.")));

	if (index_count == 0 && partial_skipped > 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no non-partial BM25 index found "
						"for this expression "
						"(%d partial index(es) skipped)",
						partial_skipped),
				 errhint("Use to_bm25query('query', "
						 "'index_name') to specify which "
						 "partial index to use.")));

	return result;
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
			-1, /* bm25query is variable-length, including private hints */
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

	if (opexpr->opno != oids->text_tpquery_operator_oid &&
		opexpr->opno != oids->textarray_tpquery_operator_oid)
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

	/*
	 * Reject constant left operands (no table column refs).
	 * Expression index operands contain Var nodes referencing
	 * table columns; a bare constant like 'hello' does not.
	 */
	if (!IsA(left, Var) && !contain_var_clause(left))
	{
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("left operand of <@> must reference "
						"a table column"),
				 errhint("Use column_name <@> bm25query, not "
						 "a constant.")));
	}

	/*
	 * If the bm25query has an explicit index, validate that the
	 * index matches the column or expression being queried.
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
							 errmsg("index \"%s\" is not on "
									"column \"%s\"",
									index_name ? index_name : "(unknown)",
									col_name ? col_name : "(unknown)"),
							 errdetail(
									 "The explicitly specified "
									 "index is not built on the "
									 "column being searched."),
							 errhint("Use an index that is built"
									 " on column \"%s\" of table"
									 " \"%s\", or omit the index"
									 " name to use automatic "
									 "index resolution.",
									 col_name ? col_name : "(unknown)",
									 table_name ? table_name : "(unknown)")));
				}
			}
		}

		/*
		 * For expressions, trust the user's explicit index
		 * specification. Full validation would require
		 * deserializing pg_index.indexprs and comparing
		 * trees, but the user explicitly named the index.
		 */
		return NULL; /* Already resolved */
	}

	/*
	 * Implicit index resolution: find a BM25 index that
	 * matches the left operand.
	 */
	if (IsA(left, Var))
	{
		index_oid = find_index_for_var((Var *)left, context);
	}
	else
	{
		Index varno;
		Oid	  relid;

		relid = get_expr_relid(left, context->query, &varno);
		if (OidIsValid(relid))
			index_oid = find_bm25_index_for_expr(
					left, relid, varno, context->oid_cache->bm25_am_oid);
		else
			index_oid = InvalidOid;
	}

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
	Const		 *text_const;
	Oid			  index_oid;
	char		 *query_text;
	TpQuery		 *tpquery;
	Const		 *tpquery_const;
	bool		  is_text_array_op =
			(opexpr->opno == oids->textarray_text_operator_oid);

	if (opexpr->opno != oids->text_text_operator_oid && !is_text_array_op)
		return NULL;

	if (list_length(opexpr->args) != 2)
		return NULL;

	left  = linitial(opexpr->args);
	right = lsecond(opexpr->args);

	if (!IsA(right, Const))
		return NULL;

	text_const = (Const *)right;

	if (text_const->consttype != TEXTOID || text_const->constisnull)
		return NULL;

	/*
	 * Reject constant left operands (no table column refs).
	 */
	if (!IsA(left, Var) && !contain_var_clause(left))
	{
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("left operand of <@> must reference "
						"a table column"),
				 errhint("Use column_name <@> 'query text', "
						 "not a constant.")));
	}

	/* Find index: plain column or expression */
	if (IsA(left, Var))
	{
		index_oid = find_index_for_var((Var *)left, context);
	}
	else
	{
		Index varno;
		Oid	  relid;

		relid = get_expr_relid(left, context->query, &varno);
		if (OidIsValid(relid))
			index_oid = find_bm25_index_for_expr(
					left, relid, varno, context->oid_cache->bm25_am_oid);
		else
			index_oid = InvalidOid;
	}

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
			is_text_array_op ? oids->textarray_tpquery_operator_oid
							 : oids->text_tpquery_operator_oid,
			left,
			(Node *)tpquery_const,
			opexpr->inputcollid,
			opexpr->location);
}

/* Forward declaration */
static void resolve_indexes_in_query(Query *query);

/*
 * Mutator function to resolve unresolved tpquery constants.
 */
static Node *
resolve_index_mutator(Node *node, void *context_arg)
{
	ResolveIndexContext *context = context_arg;
	Node				*result;

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

	/*
	 * expression_tree_mutator does not descend into SubLink
	 * subselects (they are Query nodes, not expression nodes).
	 * Process them here so implicit resolution works inside
	 * subqueries of DML statements like DELETE and UPDATE.
	 */
	if (IsA(node, SubLink))
	{
		SubLink *sublink = (SubLink *)node;

		if (sublink->subselect && IsA(sublink->subselect, Query))
			resolve_indexes_in_query((Query *)sublink->subselect);
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
 *
 * Performance note: For non-BM25 queries, this does minimal work - just a
 * cached OID lookup and a quick walk of expressions to check operator OIDs.
 * Expensive operations (syscache lookups, index resolution) only happen
 * for actual BM25 operator expressions.
 */
static void
tp_post_parse_analyze_hook(
		ParseState *pstate		pg_attribute_unused(),
		Query				   *query,
		TP_JUMBLE_STATE *jstate pg_attribute_unused())
{
	/*
	 * Skip index resolution if we're not in a valid transaction state.
	 * This happens when a statement is parsed after an error in a
	 * transaction block (e.g., ROLLBACK after a failed query). In that
	 * state, catalog lookups are forbidden.
	 */
	if (IsTransactionState())
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
	return opexpr->opno == oids->text_tpquery_operator_oid ||
		   opexpr->opno == oids->textarray_tpquery_operator_oid;
}

/*
 * Create a FuncExpr calling bm25_get_current_score().
 *
 * The function OID comes from the extension-namespace lookup cached in
 * BM25OidCache, never from a name lookup at plan time. Returns NULL if the
 * stub is unavailable so callers leave the original expression in place.
 */
static FuncExpr *
make_stub_funcexpr(BM25OidCache *oids)
{
	FuncExpr *funcexpr;

	if (!OidIsValid(oids->score_stub_func_oid))
		return NULL;

	funcexpr				 = makeNode(FuncExpr);
	funcexpr->funcid		 = oids->score_stub_func_oid;
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
			FuncExpr *stub = make_stub_funcexpr(oids);

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

typedef struct ScoreProjectionContext
{
	BM25OidCache *oids;
	PlannedStmt	 *stmt;
	bool		  in_subplan;
} ScoreProjectionContext;

/*
 * A nested scan can overwrite the backend-local score before projection.
 * User-defined functions can run scans even when marked IMMUTABLE. Permit
 * only built-in immutable functions and known scan-free stable/accessor calls.
 */
static bool
score_function_is_unsafe(Oid funcid, void *context)
{
	BM25OidCache *oids = context;

	if (funcid == oids->score_stub_func_oid ||
		funcid == oids->partition_score_func_oid ||
		funcid == oids->partition_array_score_func_oid ||
		funcid == F_TS_MATCH_TQ || funcid == F_CURRENT_SETTING_TEXT ||
		funcid == F_CURRENT_SETTING_TEXT_BOOL)
		return false;

	return funcid >= FirstNormalObjectId ||
		   func_volatile(funcid) != PROVOLATILE_IMMUTABLE;
}

static bool score_projection_is_unsafe(Node *node, void *context);

static bool
score_subplan_type_is_unsafe(Oid type)
{
	while (OidIsValid(type))
	{
		if (type >= FirstNormalObjectId || get_typtype(type) != TYPTYPE_BASE)
			return true;
		type = get_element_type(type);
	}
	return false;
}

/*
 * Index AMs and opclass support functions can execute SQL without appearing
 * in expressions. Restrict lookups to btree with built-in support functions.
 */
static bool
score_subplan_index_is_unsafe(Oid indexid, BM25OidCache *oids)
{
	Relation index	= index_open(indexid, AccessShareLock);
	bool	 unsafe = index->rd_rel->relam != BTREE_AM_OID;
	int		 nkeys	= IndexRelationGetNumberOfKeyAttributes(index);
	int		 nprocs = nkeys * index->rd_indam->amsupport;
	int		 i;

	/* Polymorphic comparators can dispatch to user-defined type functions. */
	for (i = 0; !unsafe && i < nkeys; i++)
	{
		Oid type = index->rd_opcintype[i];

		if (score_subplan_type_is_unsafe(type))
			unsafe = true;
	}
	for (i = 0; !unsafe && i < nprocs; i++)
	{
		Oid funcid = index->rd_support[i];

		if (OidIsValid(funcid) && score_function_is_unsafe(funcid, oids))
			unsafe = true;
	}
	index_close(index, AccessShareLock);
	return unsafe;
}

static bool
score_subplan_is_unsafe(Plan *plan, ScoreProjectionContext *context)
{
	if (plan == NULL)
		return false;
	check_stack_depth();

	if (score_projection_is_unsafe((Node *)plan->targetlist, context) ||
		score_projection_is_unsafe((Node *)plan->qual, context) ||
		score_projection_is_unsafe((Node *)plan->initPlan, context))
		return true;

	if (IsA(plan, SeqScan) || IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan))
	{
		Scan		  *scan = (Scan *)plan;
		RangeTblEntry *rte	= rt_fetch(scan->scanrelid, context->stmt->rtable);

		if (rte->rtekind != RTE_RELATION ||
			get_rel_relam(rte->relid) != HEAP_TABLE_AM_OID)
			return true;
	}

	switch (nodeTag(plan))
	{
	case T_Result:
		if (score_projection_is_unsafe(
					((Result *)plan)->resconstantqual, context))
			return true;
		break;
	case T_SeqScan:
	case T_Material:
		break;
	case T_IndexScan:
	{
		IndexScan *scan = (IndexScan *)plan;

		if (score_subplan_index_is_unsafe(scan->indexid, context->oids) ||
			score_projection_is_unsafe((Node *)scan->indexqual, context) ||
			score_projection_is_unsafe((Node *)scan->indexqualorig, context) ||
			score_projection_is_unsafe((Node *)scan->indexorderby, context) ||
			score_projection_is_unsafe(
					(Node *)scan->indexorderbyorig, context))
			return true;
		break;
	}
	case T_IndexOnlyScan:
	{
		IndexOnlyScan *scan = (IndexOnlyScan *)plan;

		if (score_subplan_index_is_unsafe(scan->indexid, context->oids) ||
			score_projection_is_unsafe((Node *)scan->indexqual, context) ||
			score_projection_is_unsafe((Node *)scan->recheckqual, context) ||
			score_projection_is_unsafe((Node *)scan->indexorderby, context))
			return true;
		break;
	}
	case T_Limit:
		if (((Limit *)plan)->limitOption != LIMIT_OPTION_COUNT ||
			score_projection_is_unsafe(
					((Limit *)plan)->limitOffset, context) ||
			score_projection_is_unsafe(((Limit *)plan)->limitCount, context))
			return true;
		break;
	case T_SubqueryScan:
		if (score_subplan_is_unsafe(((SubqueryScan *)plan)->subplan, context))
			return true;
		break;
	default:
		/* Uninspected expressions or executor callbacks require fallback. */
		return true;
	}

	return score_subplan_is_unsafe(plan->lefttree, context) ||
		   score_subplan_is_unsafe(plan->righttree, context);
}

static bool
score_projection_is_unsafe(Node *node, void *arg)
{
	ScoreProjectionContext *context = arg;

	if (node == NULL)
		return false;
	if (IsA(node, AlternativeSubPlan))
		return true;
	if (IsA(node, SubPlan))
	{
		SubPlan *subplan = (SubPlan *)node;
		Plan *plan = list_nth(context->stmt->subplans, subplan->plan_id - 1);
		ScoreProjectionContext subcontext = *context;

		subcontext.in_subplan = true;
		if (score_subplan_is_unsafe(plan, &subcontext))
			return true;
		return expression_tree_walker(
				node, score_projection_is_unsafe, &subcontext);
	}

	if (context->in_subplan)
	{
		/*
		 * Some expressions (domain checks, MinMaxExpr, etc.) invoke SQL
		 * functions not reported by check_functions_in_node().
		 */
		switch (nodeTag(node))
		{
		case T_List:
		case T_TargetEntry:
		case T_CaseWhen:
			break;
		case T_Var:
		case T_Const:
		case T_Param:
		case T_FuncExpr:
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_RelabelType:
		case T_CollateExpr:
		case T_CaseExpr:
		case T_CoalesceExpr:
		case T_CoerceViaIO:
		case T_ArrayExpr:
		case T_ArrayCoerceExpr:
			if (score_subplan_type_is_unsafe(exprType(node)))
				return true;
			break;
		default:
			return true;
		}
	}

	/* Scoring operators and the score accessor do not run nested scans. */
	if (!is_bm25_score_opexpr(node, context->oids) &&
		check_functions_in_node(node, score_function_is_unsafe, context->oids))
		return true;

	return expression_tree_walker(node, score_projection_is_unsafe, context);
}

static void
replace_scores_in_indexscan(IndexScan *scan, ScoreProjectionContext *context)
{
	BM25OidCache *oids = context->oids;
	Expr		 *orderby_expr;
	ListCell	 *lc;

	if (list_length(scan->indexorderbyorig) != 1 ||
		!plan_has_bm25_indexscan((Plan *)scan, oids))
		return;

	orderby_expr = linitial(scan->indexorderbyorig);
	if (!is_bm25_score_opexpr((Node *)orderby_expr, oids) ||
		score_projection_is_unsafe((Node *)scan->scan.plan.qual, context) ||
		score_projection_is_unsafe((Node *)scan->indexqualorig, context))
		return;

	foreach (lc, scan->scan.plan.targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		/* Matching expressions are removed, so cannot run nested scans. */
		if (!equal(tle->expr, orderby_expr) &&
			score_projection_is_unsafe((Node *)tle->expr, context))
			return;
	}

	/*
	 * ORDER BY may refer to a projected alias, and Append children do not
	 * retain resjunk labels. Match the expression this scan actually scored.
	 */
	foreach (lc, scan->scan.plan.targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (equal(tle->expr, orderby_expr))
		{
			FuncExpr *stub = make_stub_funcexpr(oids);

			if (stub != NULL)
				tle->expr = (Expr *)stub;
		}
	}
}

/*
 * Recursively walk the plan tree and replace BM25 score expressions.
 */
static void
replace_scores_in_plan(Plan *plan, ScoreProjectionContext *context)
{
	if (plan == NULL)
		return;

	if (IsA(plan, IndexScan))
		replace_scores_in_indexscan((IndexScan *)plan, context);
	else
		/* Preserve score reuse in projections above ordered joins. */
		replace_scores_in_targetlist(plan->targetlist, context->oids);

	/* Recurse into child plans */
	replace_scores_in_plan(plan->lefttree, context);
	replace_scores_in_plan(plan->righttree, context);

	/* Handle plan-specific children */
	switch (nodeTag(plan))
	{
	case T_Append:
	{
		Append	 *append = (Append *)plan;
		ListCell *lc;

		foreach (lc, append->appendplans)
			replace_scores_in_plan((Plan *)lfirst(lc), context);
	}
	break;

	case T_MergeAppend:
	{
		MergeAppend *merge = (MergeAppend *)plan;
		ListCell	*lc;

		foreach (lc, merge->mergeplans)
			replace_scores_in_plan((Plan *)lfirst(lc), context);
	}
	break;

	case T_SubqueryScan:
	{
		SubqueryScan *subquery = (SubqueryScan *)plan;

		replace_scores_in_plan(subquery->subplan, context);
	}
	break;

	case T_CustomScan:
	{
		CustomScan *cscan = (CustomScan *)plan;
		ListCell   *lc2;

		foreach (lc2, cscan->custom_plans)
			replace_scores_in_plan((Plan *)lfirst(lc2), context);
	}
	break;

	default:
		break;
	}
}

typedef struct PartitionScoreContext
{
	BM25OidCache *oids;
	List		 *rtable;
	Plan		 *plan;
	Query		 *query;
	List		 *query_stack;
	List		 *ordering_scores;
	bool		  row_level;
} PartitionScoreContext;

/*
 * A fixed leaf can be bound after planning without changing index pathkeys.
 * Expressions receiving rows from several leaves use a tableoid Var that
 * core carries through the plan instead.
 */
static Oid
score_expression_table(
		Plan *plan, Node *expr, List *rtable, bool include_partitioned)
{
	List *vars = pull_var_clause(
			expr,
			PVC_INCLUDE_AGGREGATES | PVC_INCLUDE_WINDOWFUNCS |
					PVC_INCLUDE_PLACEHOLDERS);
	ListCell *lc;
	Oid		  result = InvalidOid;

	foreach (lc, vars)
	{
		Var *var	   = lfirst(lc);
		Oid	 table_oid = InvalidOid;

		if (!IsA(var, Var))
		{
			result = InvalidOid;
			break;
		}
		if (var->varno == OUTER_VAR || var->varno == INNER_VAR)
		{
			Plan *child = var->varno == OUTER_VAR ? plan->lefttree
												  : plan->righttree;

			if (child != NULL)
			{
				TargetEntry *tle =
						get_tle_by_resno(child->targetlist, var->varattno);

				if (tle != NULL)
					table_oid = score_expression_table(
							child,
							(Node *)tle->expr,
							rtable,
							include_partitioned);
			}
		}
		else if (
				var->varno > 0 &&
				(unsigned int)var->varno <= (unsigned int)list_length(rtable))
		{
			RangeTblEntry *rte = rt_fetch(var->varno, rtable);

			if (rte->rtekind == RTE_RELATION &&
				(include_partitioned ||
				 rte->relkind != RELKIND_PARTITIONED_TABLE))
				table_oid = rte->relid;
			else if (IsA(plan, SubqueryScan))
			{
				Plan		*child = ((SubqueryScan *)plan)->subplan;
				TargetEntry *tle =
						get_tle_by_resno(child->targetlist, var->varattno);

				if (tle != NULL)
					table_oid = score_expression_table(
							child,
							(Node *)tle->expr,
							rtable,
							include_partitioned);
			}
		}

		/* An Append or aggregate can obscure the physical source. */
		if (!OidIsValid(table_oid) && include_partitioned &&
			var->varnosyn > 0 && var->varnosyn <= (Index)list_length(rtable))
		{
			RangeTblEntry *rte = rt_fetch(var->varnosyn, rtable);

			if (rte->rtekind == RTE_RELATION)
				table_oid = rte->relid;
		}

		if (!OidIsValid(table_oid) ||
			(OidIsValid(result) && result != table_oid))
		{
			result = InvalidOid;
			break;
		}
		result = table_oid;
	}
	list_free(vars);
	return result;
}

static Oid		get_index_table_oid(Oid index_oid);
static TpQuery *extract_tpquery_from_expr(Node *node, BM25OidCache *oids);

static bool
score_needs_partition_context(OpExpr *op, BM25OidCache *oids)
{
	Node *right;

	if (!is_bm25_score_opexpr((Node *)op, oids))
		return false;

	right = lsecond(op->args);
	/* The parser leaves to_bm25query('...', 'index') calls unfolded. */
	if (!IsA(right, Const))
		right = eval_const_expressions(NULL, right);
	if (IsA(right, Const))
	{
		Const	*value = (Const *)right;
		TpQuery *query;
		Oid		 index_oid;
		char	 relkind;

		if (value->constisnull)
			return false;
		query	  = (TpQuery *)DatumGetPointer(value->constvalue);
		index_oid = get_tpquery_index_oid(query);
		relkind	  = get_rel_relkind(index_oid);
		return relkind == RELKIND_PARTITIONED_INDEX ||
			   (relkind == RELKIND_INDEX && tpquery_is_explicit_index(query) &&
				IsA(linitial(op->args), Var) &&
				get_rel_relispartition(get_index_table_oid(index_oid)));
	}
	/* A parameter or subquery may name a parent index at run time. */
	return true;
}

static bool
has_partition_score_walker(Node *node, void *arg)
{
	if (node == NULL)
		return false;
	if (IsA(node, OpExpr) &&
		score_needs_partition_context((OpExpr *)node, arg))
		return true;
	if (IsA(node, Query))
		return query_tree_walker(
				(Query *)node, has_partition_score_walker, arg, 0);
	return expression_tree_walker(node, has_partition_score_walker, arg);
}

typedef struct ScoreSourceContext
{
	Var *source;
} ScoreSourceContext;

static bool
score_source_walker(Node *node, void *arg)
{
	ScoreSourceContext *context = arg;

	if (node == NULL)
		return false;
	if (IsA(node, Aggref) || IsA(node, WindowFunc) || IsA(node, SubLink) ||
		IsA(node, PlaceHolderVar))
		return true;
	if (IsA(node, Var))
	{
		Var *var = (Var *)node;

		if (context->source != NULL &&
			(context->source->varno != var->varno ||
			 context->source->varlevelsup != var->varlevelsup))
			return true;
		context->source = var;
		return false;
	}
	return expression_tree_walker(node, score_source_walker, arg);
}

static Var *
score_tableoid_var(Node *expr, List *query_stack)
{
	ScoreSourceContext context = {0};
	Var				  *source;
	Query			  *query;
	int				   varno;
	RangeTblEntry	  *rte;
	Var				  *result;

	if (score_source_walker(expr, &context))
		return NULL;
	source = context.source;
	if (source == NULL ||
		source->varlevelsup >= (unsigned int)list_length(query_stack))
		return NULL;

	query = list_nth(query_stack, source->varlevelsup);
	varno = source->varno;

	/*
	 * EXCLUDED has no tableoid.  ON CONFLICT evaluates it beside the
	 * conflicting row, which is in the partition the proposed row targets.
	 */
	if (query->onConflict != NULL && varno == query->onConflict->exclRelIndex)
		varno = query->resultRelation;
	if (varno <= 0 || varno > list_length(query->rtable))
		return NULL;

	/* Only scanned tables have a tableoid; a trigger's view row does not. */
	rte = rt_fetch(varno, query->rtable);
	if (rte->rtekind != RTE_RELATION ||
		(rte->relkind != RELKIND_RELATION &&
		 rte->relkind != RELKIND_PARTITIONED_TABLE &&
		 rte->relkind != RELKIND_FOREIGN_TABLE &&
		 rte->relkind != RELKIND_MATVIEW))
		return NULL;

	result =
			makeVar(varno,
					TableOidAttributeNumber,
					OIDOID,
					-1,
					InvalidOid,
					source->varlevelsup);
	result->varnullingrels = bms_copy(source->varnullingrels);
#if PG_VERSION_NUM >= 180000
	/* RETURNING old of a moved row needs the partition it left. */
	result->varreturningtype = source->varreturningtype;
#endif
	return result;
}

static Expr *
score_plan_tableoid(Node *expr, Plan *plan)
{
	List *vars = pull_var_clause(
			expr,
			PVC_INCLUDE_AGGREGATES | PVC_INCLUDE_WINDOWFUNCS |
					PVC_INCLUDE_PLACEHOLDERS);
	Index	  source = 0;
	ListCell *lc;

	foreach (lc, vars)
	{
		Var *var = lfirst(lc);

		if (!IsA(var, Var) || var->varnosyn == 0 ||
			(source != 0 && source != var->varnosyn))
		{
			list_free(vars);
			return NULL;
		}
		source = var->varnosyn;
	}
	list_free(vars);
	if (source == 0)
		return NULL;

	foreach (lc, plan->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (IsA(tle->expr, Var))
		{
			Var *var = (Var *)tle->expr;

			if (var->varnosyn == source &&
				var->varattnosyn == TableOidAttributeNumber)
				return copyObject(tle->expr);
		}
	}
	return NULL;
}

/*
 * Whether each output row of a query comes from one scanned row, so that a
 * score in its target list still has that row's partition.
 */
static bool
query_preserves_score_rows(Query *query)
{
	return query->commandType == CMD_SELECT && query->groupClause == NIL &&
		   query->groupingSets == NIL && !query->hasAggs &&
		   query->havingQual == NULL && query->distinctClause == NIL &&
		   query->setOperations == NULL;
}

/*
 * Follow a sort key through subquery and CTE columns to the score defining
 * it, which a pulled-up subquery or inlined CTE can expose to index paths.
 */
static void
collect_ordering_score(
		Node *expr, List *query_stack, PartitionScoreContext *context)
{
	Query		  *query = linitial(query_stack);
	Var			  *var;
	RangeTblEntry *rte;
	Query		  *subquery = NULL;
	List		  *substack = NIL;
	TargetEntry	  *tle;

	if (IsA(expr, OpExpr))
	{
		Var *tableoid;

		if (!score_needs_partition_context((OpExpr *)expr, context->oids))
			return;
		tableoid = score_tableoid_var(
				linitial(((OpExpr *)expr)->args), query_stack);
		if (tableoid != NULL && tableoid->varlevelsup == 0)
			context->ordering_scores =
					list_append_unique_ptr(context->ordering_scores, expr);
		return;
	}
	if (!IsA(expr, Var))
		return;
	var = (Var *)expr;
	if (var->varlevelsup != 0 || var->varattno <= 0 || var->varno <= 0 ||
		var->varno > list_length(query->rtable))
		return;

	rte = rt_fetch(var->varno, query->rtable);
	if (rte->rtekind == RTE_SUBQUERY)
	{
		subquery = rte->subquery;
		substack = lcons(subquery, list_copy(query_stack));
	}
	else if (
			rte->rtekind == RTE_CTE && !rte->self_reference &&
			rte->ctelevelsup < (Index)list_length(query_stack))
	{
		Query	 *owner = list_nth(query_stack, rte->ctelevelsup);
		ListCell *lc;

		foreach (lc, owner->cteList)
		{
			CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

			if (strcmp(cte->ctename, rte->ctename) != 0)
				continue;
			if (!cte->cterecursive)
			{
				subquery = castNode(Query, cte->ctequery);
				substack =
						lcons(subquery,
							  list_copy_tail(query_stack, rte->ctelevelsup));
			}
			break;
		}
	}
	if (subquery == NULL || !query_preserves_score_rows(subquery))
		return;

	tle = get_tle_by_resno(subquery->targetList, var->varattno);
	if (tle != NULL && !tle->resjunk)
		collect_ordering_score((Node *)tle->expr, substack, context);
}

static bool
collect_ordering_scores_walker(Node *node, void *arg)
{
	PartitionScoreContext *context = arg;

	if (node == NULL)
		return false;
	if (IsA(node, Query))
	{
		Query	 *query = (Query *)node;
		List	 *saved = context->query_stack;
		ListCell *lc;
		bool	  result;

		context->query_stack = lcons(query, list_copy(saved));
		if (query_preserves_score_rows(query))
		{
			foreach (lc, query->sortClause)
				collect_ordering_score(
						(Node *)get_sortgroupclause_expr(
								lfirst(lc), query->targetList),
						context->query_stack,
						context);
		}
		result = query_tree_walker(
				query, collect_ordering_scores_walker, arg, 0);
		context->query_stack = saved;
		return result;
	}
	return expression_tree_walker(node, collect_ordering_scores_walker, arg);
}

typedef struct RelScoreContext
{
	BM25OidCache *oids;
	int			  relid;
} RelScoreContext;

static bool
rel_score_walker(Node *node, void *arg)
{
	RelScoreContext *context = arg;

	if (node == NULL)
		return false;
	if (IsA(node, OpExpr) &&
		score_needs_partition_context((OpExpr *)node, context->oids))
	{
		ScoreSourceContext source = {0};

		if (!score_source_walker(linitial(((OpExpr *)node)->args), &source) &&
			source.source != NULL && source.source->varno == context->relid &&
			source.source->varlevelsup == 0)
			return true;
	}
	return expression_tree_walker(node, rel_score_walker, arg);
}

/*
 * A score left unbound for index ordering, or written over a pulled-up
 * subquery's column, is bound after planning.  Where a Sort, join or upper
 * projection evaluates it, the input must carry each row's tableoid.  Core
 * adds row-mark tableoid columns to processed_tlist the same way; this runs
 * before build_base_rel_tlists(), so the column reaches the rel's target.
 */
static void
tp_carry_partition_tableoid(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry  *rte;
	RelScoreContext context;
	ListCell	   *lc;
	Var			   *tableoid;

	if (partition_carry_oids == NULL || rel->reloptkind != RELOPT_BASEREL ||
		!query_preserves_score_rows(root->parse))
		return;
	rte = planner_rt_fetch(rel->relid, root);
	if (rte->rtekind != RTE_RELATION ||
		rte->relkind != RELKIND_PARTITIONED_TABLE || !rte->inh)
		return;

	context.oids  = partition_carry_oids;
	context.relid = (int)rel->relid;
	if (!rel_score_walker((Node *)root->processed_tlist, &context))
		return;

	foreach (lc, root->processed_tlist)
	{
		Var *var = (Var *)lfirst_node(TargetEntry, lc)->expr;

		if (IsA(var, Var) && var->varno == context.relid &&
			var->varattno == TableOidAttributeNumber && var->varlevelsup == 0)
			return;
	}

	tableoid = makeVar(
			rel->relid, TableOidAttributeNumber, OIDOID, -1, InvalidOid, 0);
	root->processed_tlist =
			lappend(root->processed_tlist,
					makeTargetEntry(
							(Expr *)tableoid,
							list_length(root->processed_tlist) + 1,
							pstrdup("bm25_partition_tableoid"),
							true));
}

#if PG_VERSION_NUM >= 190000
static void
tp_build_simple_rel_hook(
		PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (prev_build_simple_rel_hook)
		prev_build_simple_rel_hook(root, rel, rte);
	tp_carry_partition_tableoid(root, rel);
}
#else
static void
tp_get_relation_info_hook(
		PlannerInfo *root,
		Oid			 relationObjectId,
		bool		 inhparent,
		RelOptInfo	*rel)
{
	if (prev_get_relation_info_hook)
		prev_get_relation_info_hook(root, relationObjectId, inhparent, rel);
	if (inhparent)
		tp_carry_partition_tableoid(root, rel);
}
#endif

static Node *
bind_partition_score_mutator(Node *node, void *arg)
{
	PartitionScoreContext *context = arg;
	OpExpr				  *op;
	Expr				  *table_expr = NULL;
	Oid					   funcid;
	bool				   ordering;
	TpQuery				  *query;

	if (node == NULL)
		return NULL;
	if (IsA(node, Query))
	{
		PartitionScoreContext subcontext = *context;
		Query				 *query		 = (Query *)node;

		subcontext.query = query;
		/* lcons() extends a list in place, which later siblings would see. */
		subcontext.query_stack = lcons(query, list_copy(context->query_stack));
		subcontext.row_level   = false;
		return (Node *)query_tree_mutator(
				query, bind_partition_score_mutator, &subcontext, 0);
	}

	/*
	 * Keep ordering operators visible to index path selection.  The tableoid
	 * carried by tp_carry_partition_tableoid() binds any that a Sort or
	 * projection ultimately evaluates.
	 */
	ordering = list_member_ptr(context->ordering_scores, node);

	if (context->query != NULL && !context->row_level &&
		(IsA(node, Aggref) || node == (Node *)context->query->jointree))
	{
		PartitionScoreContext rowcontext = *context;

		rowcontext.row_level = true;
		return bind_partition_score_mutator(node, &rowcontext);
	}

	node = expression_tree_mutator(node, bind_partition_score_mutator, arg);
	if (ordering || !IsA(node, OpExpr))
		return node;
	op = (OpExpr *)node;
	if (!score_needs_partition_context(op, context->oids))
		return node;
	if (context->query != NULL && !context->row_level &&
		(context->query->hasAggs || context->query->groupClause != NIL))
		return node;

	query = extract_tpquery_from_expr(node, context->oids);
	if (query != NULL && tpquery_is_explicit_index(query) &&
		IsA(linitial(op->args), Var) &&
		get_rel_relkind(query->index_oid) == RELKIND_INDEX)
	{
		Oid table_oid;

		/* Validate a named leaf only after partition pruning has run. */
		if (context->query != NULL)
			return node;

		table_oid = score_expression_table(
				context->plan, linitial(op->args), context->rtable, true);

		if (OidIsValid(table_oid) &&
			table_oid != get_index_table_oid(query->index_oid))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("index \"%s\" cannot score rows from table "
							"\"%s\"",
							get_rel_name(query->index_oid),
							get_rel_name(table_oid)),
					 errdetail(
							 "An explicitly named leaf index can only "
							 "score rows from its own partition."),
					 errhint("Restrict the query to that partition, or "
							 "name the parent index.")));
		return node;
	}

	funcid = op->opno == context->oids->textarray_tpquery_operator_oid
				   ? context->oids->partition_array_score_func_oid
				   : context->oids->partition_score_func_oid;
	/* An older extension SQL version has no partition-aware overload yet. */
	if (!OidIsValid(funcid))
		return node;

	if (context->query != NULL)
		table_expr = (Expr *)
				score_tableoid_var(linitial(op->args), context->query_stack);
	else
	{
		Oid table_oid = score_expression_table(
				context->plan, linitial(op->args), context->rtable, false);

		if (OidIsValid(table_oid))
			table_expr = (Expr *)makeConst(
					OIDOID,
					-1,
					InvalidOid,
					sizeof(Oid),
					ObjectIdGetDatum(table_oid),
					false,
					true);
		else
			table_expr =
					score_plan_tableoid(linitial(op->args), context->plan);
	}

	if (table_expr == NULL)
		return node;

	/*
	 * The overload takes internal so that SQL callers, which cannot construct
	 * one, cannot name a partition; the relabeled OID needs no conversion.
	 */
	table_expr = (Expr *)makeRelabelType(
			table_expr, INTERNALOID, -1, InvalidOid, COERCE_IMPLICIT_CAST);

	return (Node *)makeFuncExpr(
			funcid,
			FLOAT8OID,
			lappend(op->args, table_expr),
			op->opcollid,
			op->inputcollid,
			COERCE_EXPLICIT_CALL);
}

static void
bind_partition_scores_in_plan(Plan *plan, List *rtable, BM25OidCache *oids)
{
	PartitionScoreContext context =
			{.oids = oids, .rtable = rtable, .plan = plan};
	List	 *children = NIL;
	ListCell *lc;

	if (plan == NULL)
		return;

	plan->targetlist = (List *)
			bind_partition_score_mutator((Node *)plan->targetlist, &context);
	plan->qual = (List *)
			bind_partition_score_mutator((Node *)plan->qual, &context);
	if (IsA(plan, NestLoop) || IsA(plan, MergeJoin) || IsA(plan, HashJoin))
		((Join *)plan)->joinqual = (List *)bind_partition_score_mutator(
				(Node *)((Join *)plan)->joinqual, &context);
	if (IsA(plan, Result))
		((Result *)plan)->resconstantqual = bind_partition_score_mutator(
				((Result *)plan)->resconstantqual, &context);
	if (IsA(plan, HashJoin))
	{
		HashJoin *join = (HashJoin *)plan;

		join->hashclauses = (List *)bind_partition_score_mutator(
				(Node *)join->hashclauses, &context);
		join->hashkeys = (List *)
				bind_partition_score_mutator((Node *)join->hashkeys, &context);
	}
	if (IsA(plan, Hash))
		((Hash *)plan)->hashkeys = (List *)bind_partition_score_mutator(
				(Node *)((Hash *)plan)->hashkeys, &context);
	if (IsA(plan, MergeJoin))
		((MergeJoin *)plan)->mergeclauses = (List *)
				bind_partition_score_mutator(
						(Node *)((MergeJoin *)plan)->mergeclauses, &context);

	bind_partition_scores_in_plan(plan->lefttree, rtable, oids);
	bind_partition_scores_in_plan(plan->righttree, rtable, oids);

	if (IsA(plan, Append))
		children = ((Append *)plan)->appendplans;
	else if (IsA(plan, MergeAppend))
		children = ((MergeAppend *)plan)->mergeplans;
	else if (IsA(plan, CustomScan))
		children = ((CustomScan *)plan)->custom_plans;
	else if (IsA(plan, SubqueryScan))
		bind_partition_scores_in_plan(
				((SubqueryScan *)plan)->subplan, rtable, oids);

	foreach (lc, children)
		bind_partition_scores_in_plan(lfirst(lc), rtable, oids);
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

		if ((opexpr->opno == oids->text_tpquery_operator_oid ||
			 opexpr->opno == oids->textarray_tpquery_operator_oid) &&
			list_length(opexpr->args) == 2)
		{
			Node *right = lsecond(opexpr->args);

			if (IsA(right, FuncExpr))
				right = eval_const_expressions(NULL, right);

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
 * Check if scan_index is a descendant of specified_index via partitioned
 * index inheritance or regular table inheritance.
 *
 * For partitioned indexes, walks up the index inheritance chain.
 * For regular indexes, checks if the scan index's table inherits
 * from the specified index's table (PG17 planner may choose child
 * table indexes).
 */
static bool
is_child_partition_index(Oid specified_index_oid, Oid scan_index_oid)
{
	char relkind;
	Oid	 current_oid;
	int	 depth;

	/*
	 * Check if specified index is a partitioned index or a regular
	 * index on an inheritance parent table.  For regular inheritance,
	 * PG17's planner may choose the child table's index instead of
	 * the parent's.
	 */
	relkind = get_rel_relkind(specified_index_oid);
	if (relkind != RELKIND_PARTITIONED_INDEX && relkind != RELKIND_INDEX)
		return false;

	/*
	 * For regular (non-partitioned) indexes, check table-level
	 * inheritance: does the scan index's table inherit from the
	 * specified index's table?
	 */
	if (relkind == RELKIND_INDEX)
	{
		Oid specified_table = get_index_table_oid(specified_index_oid);
		Oid scan_table		= get_index_table_oid(scan_index_oid);

		if (!OidIsValid(specified_table) || !OidIsValid(scan_table))
			return false;

		/* Must be a different table (child), not the same table */
		if (specified_table == scan_table)
			return false;

		return list_member_oid(
				find_all_inheritors(specified_table, NoLock, NULL),
				scan_table);
	}

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

static IndexOptInfo *find_index_opt_info(RelOptInfo *rel, Oid index_oid);

/*
 * Cache index identity, not usability: ordering and predicates are per path.
 */
static IndexOptInfo *
find_required_index_opt_info(RelOptInfo *rel, Oid required_index)
{
	IndexOptInfo *indexinfo;
	ListCell	 *lc;

	if (current_planning_context != NULL)
	{
		foreach (lc, current_planning_context->index_matches)
		{
			ExplicitIndexMatch *match = (ExplicitIndexMatch *)lfirst(lc);

			if (match->rel == rel &&
				match->required_index_oid == required_index)
				return match->indexinfo;
		}
	}

	indexinfo = find_index_opt_info(rel, required_index);
	if (indexinfo == NULL &&
		get_rel_relkind(required_index) == RELKIND_PARTITIONED_INDEX)
	{
		foreach (lc, rel->indexlist)
		{
			IndexOptInfo *candidate = lfirst_node(IndexOptInfo, lc);

			if (is_child_partition_index(required_index, candidate->indexoid))
			{
				indexinfo = candidate;
				break;
			}
		}
	}

	if (current_planning_context != NULL)
	{
		MemoryContext old_context = MemoryContextSwitchTo(
				current_planning_context->cache_context);
		ExplicitIndexMatch *match = palloc(sizeof(ExplicitIndexMatch));

		match->rel				  = rel;
		match->required_index_oid = required_index;
		match->indexinfo		  = indexinfo;
		current_planning_context->index_matches =
				lappend(current_planning_context->index_matches, match);
		MemoryContextSwitchTo(old_context);
	}

	return indexinfo;
}

static bool
index_matches_path_ordering(
		IndexPath *path, IndexOptInfo *candidate, Oid bm25_am_oid)
{
	OpExpr *orderby;

	if (candidate->relam != bm25_am_oid || !candidate->amcanorderbyop ||
		candidate->nkeycolumns != path->indexinfo->nkeycolumns ||
		candidate->indexkeys[0] != path->indexinfo->indexkeys[0] ||
		!equal(candidate->indexprs, path->indexinfo->indexprs))
		return false;

	orderby = linitial_node(OpExpr, path->indexorderbys);
	if (OidIsValid(candidate->indexcollations[0]) &&
		candidate->indexcollations[0] != orderby->inputcollid)
		return false;

	if (candidate->opfamily[0] != path->indexinfo->opfamily[0] &&
		get_op_opfamily_sortfamily(orderby->opno, candidate->opfamily[0]) !=
				linitial_node(PathKey, path->path.pathkeys)->pk_opfamily)
		return false;

	return true;
}

/*
 * Constrain paths using their folded ordering query.
 * Reject a mismatched path only if the named index is usable on this relation;
 * otherwise leave the path for the existing explicit-index validation.
 */
bool
tp_indexpath_requires_other_index(IndexPath *path)
{
	BM25OidCache  oids;
	TpQuery		 *query;
	Oid			  required_index;
	IndexOptInfo *candidate;

	if (!get_bm25_oids(&oids))
		return false;

	query = find_tpquery_in_list(path->indexorderbys, &oids);
	if (query == NULL || !tpquery_is_explicit_index(query))
		return false;

	required_index = get_tpquery_index_oid(query);
	if (!OidIsValid(required_index) ||
		required_index == path->indexinfo->indexoid)
		return false;

	/* Regular table inheritance has no corresponding index hierarchy. */
	if (get_rel_relkind(required_index) == RELKIND_INDEX &&
		is_child_partition_index(required_index, path->indexinfo->indexoid))
		return false;

	candidate =
			find_required_index_opt_info(path->indexinfo->rel, required_index);
	if (candidate == NULL || candidate == path->indexinfo ||
		(candidate->indpred != NIL && !candidate->predOK) ||
		!index_matches_path_ordering(path, candidate, oids.bm25_am_oid))
		return false;

	return true;
}

/*
 * Walker context for collecting explicit index requirements from a query.
 */
typedef struct CollectExplicitIndexContext
{
	PlannerInfo	 *root;
	BM25OidCache *oid_cache;
	List		 *requirements; /* List of ExplicitIndexRequirement */
} CollectExplicitIndexContext;

/* Detect BM25 operators, including those in nested queries. */
static bool
has_bm25_operator_walker(Node *node, void *context)
{
	BM25OidCache *oids = context;

	if (node == NULL)
		return false;

	if (IsA(node, OpExpr))
	{
		OpExpr *opexpr = (OpExpr *)node;

		if (opexpr->opno == oids->text_tpquery_operator_oid ||
			opexpr->opno == oids->textarray_tpquery_operator_oid ||
			opexpr->opno == oids->text_text_operator_oid ||
			opexpr->opno == oids->textarray_text_operator_oid)
			return true;
	}

	if (IsA(node, Query))
		return query_tree_walker(
				(Query *)node, has_bm25_operator_walker, oids, 0);

	return expression_tree_walker(node, has_bm25_operator_walker, oids);
}

/*
 * Walker to find explicit index requirements in query expressions.
 */
static bool
collect_explicit_indexes_walker(Node *node, void *context_arg)
{
	CollectExplicitIndexContext *context = context_arg;

	if (node == NULL)
		return false;

	/* Nested requirements must not constrain another query's index paths. */
	if (IsA(node, Query))
		return false;

	if (IsA(node, OpExpr))
	{
		OpExpr *opexpr = (OpExpr *)node;

		if ((opexpr->opno == context->oid_cache->text_tpquery_operator_oid ||
			 opexpr->opno ==
					 context->oid_cache->textarray_tpquery_operator_oid) &&
			list_length(opexpr->args) == 2)
		{
			Node *left	= linitial(opexpr->args);
			Node *right = lsecond(opexpr->args);

			/*
			 * Try to fold FuncExpr (e.g., to_bm25query())
			 * to a Const.
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

					/*
					 * Only collect explicitly specified indexes
					 * (from to_bm25query with index name).
					 */
					if (tpquery_is_explicit_index(tpquery))
					{
						Oid index_oid = get_tpquery_index_oid(tpquery);

						if (OidIsValid(index_oid))
						{
							Oid table_oid = get_index_table_oid(index_oid);

							if (OidIsValid(table_oid))
							{
								ExplicitIndexRequirement *req;
								Bitmapset				 *relids;
								int						  rti;

								relids = pull_varnos(context->root, left);
								if (!bms_get_singleton_member(relids, &rti))
								{
									bms_free(relids);
									return false;
								}
								bms_free(relids);

								req = palloc(sizeof(ExplicitIndexRequirement));
								req->relid				= table_oid;
								req->rti				= rti;
								req->required_index_oid = index_oid;

								if (IsA(left, Var))
								{
									Var *var = (Var *)left;

									req->attnum = var->varattno;
									req->expr	= NULL;
								}
								else
								{
									req->attnum = 0;
									req->expr	= left;
								}
								context->requirements =
										lappend(context->requirements, req);
							}
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
collect_explicit_index_requirements(PlannerInfo *root, BM25OidCache *oid_cache)
{
	CollectExplicitIndexContext context;

	context.root		 = root;
	context.oid_cache	 = oid_cache;
	context.requirements = NIL;

	/* Collect requirements at this query level only. */
	query_tree_walker(
			root->parse, collect_explicit_indexes_walker, &context, 0);

	return context.requirements;
}

/* Pullup and expression preprocessing are complete before base paths are
 * built. Child queries have separate roots, so cache even an empty result
 * once per actual PlannerInfo, rather than walking the query for each alias.
 * The private context belongs to this planner invocation, including errors. */
static List *
get_explicit_index_requirements(PlannerInfo *root, PlanningContext *context)
{
	ExplicitIndexRequirements *entry;
	MemoryContext			   old_context;
	List					  *requirements;

	if (context->first.root == root)
		return context->first.requirements;

	if (context->other_roots != NULL)
	{
		entry = hash_search(context->other_roots, &root, HASH_FIND, NULL);
		if (entry != NULL)
			return entry->requirements;
	}

	old_context = MemoryContextSwitchTo(context->cache_context);
	PG_TRY();
	{
		requirements =
				collect_explicit_index_requirements(root, context->oid_cache);

		/* Collect before publishing: collection can invoke the planner. */
		if (context->first.root == NULL)
		{
			context->first.requirements = requirements;
			context->first.root			= root;
		}
		else
		{
			if (context->other_roots == NULL)
			{
				HASHCTL ctl = {0};

				ctl.keysize			 = sizeof(root);
				ctl.entrysize		 = sizeof(ExplicitIndexRequirements);
				ctl.hcxt			 = context->cache_context;
				context->other_roots = hash_create(
						"BM25 query requirements",
						8,
						&ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			}
			entry = hash_search(context->other_roots, &root, HASH_ENTER, NULL);
			entry->requirements = requirements;
		}
	}
	PG_FINALLY();
	{
		MemoryContextSwitchTo(old_context);
	}
	PG_END_TRY();

	return requirements;
}

/*
 * Find an explicit index requirement for a given relation and
 * column or expression.  For plain columns (attnum > 0), matches
 * by range table entry, relid and attnum. For expression indexes (attnum ==
 * 0), matches by relid and expression tree equality. Returns NULL if no
 * explicit requirement exists.
 */
static ExplicitIndexRequirement *
find_explicit_requirement(
		List	  *requirements,
		Index	   rti,
		Oid		   relid,
		AttrNumber attnum,
		Node	  *expr)
{
	ListCell *lc;

	foreach (lc, requirements)
	{
		ExplicitIndexRequirement *req = (ExplicitIndexRequirement *)lfirst(lc);

		if (req->relid != relid || req->rti != rti)
			continue;

		if (attnum != 0 && req->attnum == attnum)
			return req;

		if (attnum == 0 && req->attnum == 0 && expr != NULL &&
			req->expr != NULL && equal(expr, req->expr))
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
 * Fix BM25 IndexPaths in a path list to use the explicitly specified index.
 *
 * For each BM25 IndexPath, finds the matching explicit requirement by
 * column and swaps the indexinfo if needed.
 */
static void
fix_bm25_indexpaths(
		List	   *pathlist,
		RelOptInfo *rel,
		Index		rti,
		Oid			relid,
		Oid			bm25_am_oid,
		List	   *requirements)
{
	ListCell *lc;

	foreach (lc, pathlist)
	{
		Path					 *path = (Path *)lfirst(lc);
		IndexPath				 *indexpath;
		Oid						  index_oid;
		AttrNumber				  attnum;
		ExplicitIndexRequirement *req;
		IndexOptInfo			 *correct_indexinfo;

		if (!IsA(path, IndexPath))
			continue;

		index_oid = get_path_bm25_index_oid(path, bm25_am_oid);
		if (!OidIsValid(index_oid))
			continue;

		indexpath = (IndexPath *)path;
		if (indexpath->indexinfo->nkeycolumns < 1)
			continue;
		attnum = indexpath->indexinfo->indexkeys[0];

		{
			Node *idx_expr = NULL;

			if (attnum == 0 && indexpath->indexinfo->indexprs)
				idx_expr = (Node *)linitial(indexpath->indexinfo->indexprs);

			req = find_explicit_requirement(
					requirements, rti, relid, attnum, idx_expr);
		}
		if (req == NULL)
			continue;

		if (index_oid == req->required_index_oid)
			continue;

		if (is_child_partition_index(req->required_index_oid, index_oid))
			continue;

		correct_indexinfo = find_index_opt_info(rel, req->required_index_oid);
		if (correct_indexinfo == NULL ||
			(correct_indexinfo->indpred != NIL && !correct_indexinfo->predOK))
			continue;

		if (indexpath->indexorderbys != NIL &&
			!index_matches_path_ordering(
					indexpath, correct_indexinfo, bm25_am_oid))
			continue;

		indexpath->indexinfo = correct_indexinfo;
	}
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
 * current_planning_context will be NULL when the query has no BM25 operators.
 * The overhead for non-BM25 queries is just a single pointer comparison.
 */
static void
tp_set_rel_pathlist_hook(
		PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	List		 *requirements;
	BM25OidCache *oids;
	ListCell	 *cell;
	bool		  has_bm25_index = false;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	/* Only process base relations */
	if (rel->reloptkind != RELOPT_BASEREL)
		return;

	/* Check if we have planning context */
	if (current_planning_context == NULL)
		return;

	/* Subqueries are planned by subquery_planner, without re-entering our
	 * planner hook. Collect from the actual root after pullup has adjusted
	 * Vars, and keep aliases distinct by their range table entry. */
	oids = current_planning_context->oid_cache;
	foreach (cell, rel->indexlist)
	{
		if (lfirst_node(IndexOptInfo, cell)->relam == oids->bm25_am_oid)
		{
			has_bm25_index = true;
			break;
		}
	}
	if (!has_bm25_index)
		return;
	requirements =
			get_explicit_index_requirements(root, current_planning_context);
	if (requirements == NIL)
		return;
	fix_bm25_indexpaths(
			rel->pathlist,
			rel,
			rti,
			rte->relid,
			oids->bm25_am_oid,
			requirements);
	fix_bm25_indexpaths(
			rel->partial_pathlist,
			rel,
			rti,
			rte->relid,
			oids->bm25_am_oid,
			requirements);
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
		/*
		 * Allow partitioned indexes: if specified is parent and scan is child
		 */
		if (is_child_partition_index(specified_index_oid, indexscan->indexid))
			return; /* Child partition index - allowed */

		/*
		 * When the user names an index explicitly via
		 * to_bm25query(text, index_name), that index must be used for the
		 * scan to ensure consistent tokenization; error if the planner chose
		 * a different one. Implicitly resolved indexes (e.g. text <@> text)
		 * impose no such constraint, so no diagnostic is emitted.
		 */
		if (tpquery_is_explicit_index(tpquery))
		{
			char *specified_name = get_rel_name(specified_index_oid);
			char *scan_name		 = get_rel_name(indexscan->indexid);

			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("query specifies index \"%s\" but planner chose "
							"index \"%s\"",
							specified_name ? specified_name : "(unknown)",
							scan_name ? scan_name : "(unknown)"),
					 errdetail(
							 "When an explicit index is specified in "
							 "to_bm25query(), that index must be used for "
							 "the scan to ensure consistent tokenization."),
					 errhint("Use a planner hint to force the specified "
							 "index, or remove the explicit index name to let "
							 "the planner choose automatically.")));
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
 *
 * PG19 added a trailing ExplainState *es parameter to planner_hook,
 * standard_planner() and prev planner hooks.  These macros splice that
 * parameter into the signature and the pass-through calls only when
 * building against PG19+.
 */
#if PG_VERSION_NUM >= 190000
#define TP_PLANNER_HOOK_DECL_EXTRA , ExplainState *es
#define TP_PLANNER_HOOK_PASS_EXTRA , es
#else
#define TP_PLANNER_HOOK_DECL_EXTRA
#define TP_PLANNER_HOOK_PASS_EXTRA
#endif

static bool
tp_limit_const_value(Node *node, int64 *value)
{
	Const *c;

	if (node == NULL || !IsA(node, Const))
		return false;
	c = (Const *)node;
	if (c->constisnull || c->consttype != INT8OID)
		return false;
	*value = DatumGetInt64(c->constvalue);
	return true;
}

static bool
tp_limit_k(Limit *limit, int64 *k)
{
	int64 count;
	int64 offset = 0;

	if (!tp_limit_const_value(limit->limitCount, &count) || count <= 0)
		return false;
	if (limit->limitOffset != NULL &&
		!tp_limit_const_value(limit->limitOffset, &offset))
		return false;
	if (offset < 0)
		return false;
	if (count >= INT_MAX - offset)
		return false;
	*k = count + offset;
	return true;
}

static void
tp_attach_seed_hint(
		IndexScan *scan, Limit *limit, List *rtable, BM25OidCache *oids)
{
	ListCell	  *lc;
	int64		   k;
	RangeTblEntry *rte;
	Relation	   heap;
	double		   selectivity = 0.0;

	if (list_length(scan->indexorderby) != 1 || scan->indexqual != NIL ||
		!tp_limit_k(limit, &k) || scan->scan.scanrelid <= 0)
		return;
	if (get_rel_relam(scan->indexid) != oids->bm25_am_oid)
		return;
	rte = rt_fetch(scan->scan.scanrelid, rtable);
	if (rte == NULL || !OidIsValid(rte->relid))
		return;
	heap = table_open(rte->relid, AccessShareLock);
	if (scan->scan.plan.qual != NIL && heap->rd_rel->reltuples > 0)
		selectivity = scan->scan.plan.plan_rows / heap->rd_rel->reltuples;
	table_close(heap, AccessShareLock);

	foreach (lc, scan->indexorderby)
	{
		Node *expr = (Node *)lfirst(lc);
		if (IsA(expr, OpExpr) && list_length(((OpExpr *)expr)->args) == 2)
		{
			Node *right = lsecond(((OpExpr *)expr)->args);
			if (IsA(right, Const) &&
				((Const *)right)->consttype == oids->tpquery_type_oid &&
				!((Const *)right)->constisnull)
			{
				Const	*original = (Const *)right;
				TpQuery *query	  = (TpQuery *)DatumGetPointer(
						   original->constvalue);
				TpQuery *hinted =
						tpquery_copy_with_seed_hint(query, k, selectivity);

				Const *replacement = makeConst(
						original->consttype,
						original->consttypmod,
						original->constcollid,
						-1,
						PointerGetDatum(hinted),
						false,
						false);

				replacement->location = original->location;
				/* Original nodes may be shared; their context owns them. */
				lsecond(((OpExpr *)expr)->args) = replacement;
			}
		}
	}
}

static void
tp_attach_seed_hints(Plan *plan, List *rtable, BM25OidCache *oids)
{
	ListCell *lc;
	if (plan == NULL)
		return;
	if (IsA(plan, Limit) && plan->lefttree != NULL &&
		IsA(plan->lefttree, IndexScan))
		tp_attach_seed_hint(
				(IndexScan *)plan->lefttree, (Limit *)plan, rtable, oids);
	tp_attach_seed_hints(plan->lefttree, rtable, oids);
	tp_attach_seed_hints(plan->righttree, rtable, oids);
	switch (nodeTag(plan))
	{
	case T_Append:
		foreach (lc, ((Append *)plan)->appendplans)
			tp_attach_seed_hints(lfirst(lc), rtable, oids);
		break;
	case T_MergeAppend:
		foreach (lc, ((MergeAppend *)plan)->mergeplans)
			tp_attach_seed_hints(lfirst(lc), rtable, oids);
		break;
	case T_SubqueryScan:
		tp_attach_seed_hints(((SubqueryScan *)plan)->subplan, rtable, oids);
		break;
	case T_CustomScan:
		foreach (lc, ((CustomScan *)plan)->custom_plans)
			tp_attach_seed_hints(lfirst(lc), rtable, oids);
		break;
	default:
		break;
	}
}

static PlannedStmt *
tp_planner_hook(
		Query					 *parse,
		const char				 *query_string,
		int						  cursorOptions,
		ParamListInfo boundParams TP_PLANNER_HOOK_DECL_EXTRA)
{
	PlannedStmt		*result;
	BM25OidCache	 oid_cache;
	PlanningContext *planning_context = NULL;
	MemoryContext	 planning_cache	  = NULL;
	MemoryContext	 caller_context	  = CurrentMemoryContext;
	PlanningContext *saved_context;
	BM25OidCache	*saved_carry_oids;
	bool			 query_has_bm25_operators;
	bool			 query_has_partition_scores;

	/* Get BM25 OIDs - if extension not installed, just pass through */
	if (!get_bm25_oids(&oid_cache))
	{
		if (prev_planner_hook)
			return prev_planner_hook(
					parse,
					query_string,
					cursorOptions,
					boundParams TP_PLANNER_HOOK_PASS_EXTRA);
		else
			return standard_planner(
					parse,
					query_string,
					cursorOptions,
					boundParams TP_PLANNER_HOOK_PASS_EXTRA);
	}

	/* Each recursive planner call owns its context, including calls that
	 * contain no BM25 operators. Query-level requirements are collected in
	 * the path hook from the PlannerInfo being processed. */
	query_has_bm25_operators =
			has_bm25_operator_walker((Node *)parse, &oid_cache);
	query_has_partition_scores =
			query_has_bm25_operators &&
			has_partition_score_walker((Node *)parse, &oid_cache);
	if (query_has_partition_scores)
	{
		PartitionScoreContext context = {.oids = &oid_cache};

		collect_ordering_scores_walker((Node *)parse, &context);
		parse = (Query *)bind_partition_score_mutator((Node *)parse, &context);
	}

	if (query_has_bm25_operators)
	{
		planning_cache = AllocSetContextCreate(
				CurrentMemoryContext,
				"BM25 planner requirements",
				ALLOCSET_SMALL_SIZES);
		planning_context = MemoryContextAllocZero(
				planning_cache, sizeof(*planning_context));
		planning_context->oid_cache		= &oid_cache;
		planning_context->cache_context = planning_cache;
	}
	saved_context			 = current_planning_context;
	current_planning_context = planning_context;

	/* A nested planner call restores its caller's carry state. */
	saved_carry_oids	 = partition_carry_oids;
	partition_carry_oids = query_has_partition_scores ? &oid_cache : NULL;

	/* Call previous hook or standard planner */
	PG_TRY();
	{
		if (prev_planner_hook)
			result = prev_planner_hook(
					parse,
					query_string,
					cursorOptions,
					boundParams TP_PLANNER_HOOK_PASS_EXTRA);
		else
			result = standard_planner(
					parse,
					query_string,
					cursorOptions,
					boundParams TP_PLANNER_HOOK_PASS_EXTRA);
	}
	PG_FINALLY();
	{
		partition_carry_oids	 = saved_carry_oids;
		current_planning_context = saved_context;
		if (planning_cache != NULL)
		{
			MemoryContextSwitchTo(caller_context);
			MemoryContextDelete(planning_cache);
		}
	}
	PG_END_TRY();

	/*
	 * Post-process the plan if it may have BM25 index scans.
	 *
	 * Performance optimization: Only check for BM25 IndexScans if the
	 * current query contains BM25 operators. This avoids expensive
	 * syscache lookups in plan_has_bm25_indexscan() for non-BM25 queries.
	 */
	if (query_has_bm25_operators && result->planTree != NULL &&
		plan_has_bm25_indexscan(result->planTree, &oid_cache))
	{
		ScoreProjectionContext context = {.oids = &oid_cache, .stmt = result};

		/*
		 * Validate implicit index resolution - for explicit indexes, the
		 * set_rel_pathlist_hook should have already forced the correct index.
		 * This validation errors when an explicitly named index differs from
		 * the one the planner scanned; implicit resolution mismatches are
		 * allowed and emit no diagnostic.
		 */
		validate_explicit_index_usage(result->planTree, &oid_cache);

		replace_scores_in_plan(result->planTree, &context);
	}

	if (query_has_partition_scores)
	{
		ListCell *lc;

		bind_partition_scores_in_plan(
				result->planTree, result->rtable, &oid_cache);
		foreach (lc, result->subplans)
			bind_partition_scores_in_plan(
					lfirst(lc), result->rtable, &oid_cache);
	}

	/*
	 * Inspect the actual plan, including separately stored SubPlans.
	 * A saved query can be planned after another statement has reset the
	 * post-parse flag, so that flag cannot gate hint attachment.
	 */
	if (result->planTree != NULL)
	{
		ListCell *lc;

		tp_attach_seed_hints(result->planTree, result->rtable, &oid_cache);
		foreach (lc, result->subplans)
			tp_attach_seed_hints(
					(Plan *)lfirst(lc), result->rtable, &oid_cache);
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

#if PG_VERSION_NUM >= 190000
	prev_build_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook	   = tp_build_simple_rel_hook;
#else
	prev_get_relation_info_hook = get_relation_info_hook;
	get_relation_info_hook		= tp_get_relation_info_hook;
#endif
}
