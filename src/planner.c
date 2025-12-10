/*
 * planner_hook.c - Planner hook for implicit BM25 index resolution
 *
 * When a query uses the <@> operator with an unresolved tpquery (index_oid
 * is InvalidOid), this hook identifies the column being scored and finds
 * a suitable BM25 index to use.
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
#include <optimizer/planner.h>
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

/* Previous planner hook in chain */
static planner_hook_type prev_planner_hook = NULL;

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
 * Syscache invalidation callback - reset cache when pg_am or pg_type changes.
 * This handles CREATE/DROP EXTENSION scenarios.
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
 * Context for query tree mutation
 */
typedef struct ResolveIndexContext
{
	Query		 *query;	 /* The query being processed */
	BM25OidCache *oid_cache; /* Looked-up OIDs for this query */
} ResolveIndexContext;

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

	/*
	 * Look up bm25query type using the search path. This handles cases where
	 * the extension is installed in a non-public schema.
	 */
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

	elog(DEBUG2,
		 "tp_planner_hook: bm25_am=%u, tpquery_type=%u, "
		 "text_tpquery_op=%u, text_text_op=%u",
		 cache->bm25_am_oid,
		 cache->tpquery_type_oid,
		 cache->text_tpquery_operator_oid,
		 cache->text_text_operator_oid);

	return true;
}

/*
 * Get BM25 OIDs with caching.
 * Uses backend-local cache to avoid repeated syscache lookups.
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
		/* Fall through to lookup */
		break;
	}

	/* Do the actual lookup */
	if (!lookup_bm25_oids_internal(cache))
	{
		oid_cache_state = CACHE_NOT_FOUND;
		return false;
	}

	/* Cache the result */
	cached_oids		= *cache;
	oid_cache_state = CACHE_VALID;
	return true;
}

/*
 * Internal helper: Find BM25 index with provided bm25_am_oid
 */
static Oid
find_bm25_index_for_column_internal(
		Oid relid, AttrNumber attnum, Oid bm25_am_oid)
{
	Relation	indexRelation;
	SysScanDesc scan;
	HeapTuple	indexTuple;
	Oid			result		= InvalidOid;
	int			index_count = 0;
	ScanKeyData scanKey;

	if (!OidIsValid(bm25_am_oid))
		return InvalidOid;

	/* Scan pg_index for indexes on this relation */
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

		/* Check if this index uses the bm25 access method */
		classTuple = SearchSysCache1(RELOID, ObjectIdGetDatum(indexOid));
		if (HeapTupleIsValid(classTuple))
		{
			Form_pg_class classForm = (Form_pg_class)GETSTRUCT(classTuple);

			if (classForm->relam == bm25_am_oid)
			{
				/*
				 * For partitioned indexes (RELKIND_PARTITIONED_INDEX),
				 * return the parent index OID. The executor will map this
				 * to partition indexes at scan time via oid_inherits_from()
				 * in tp_rescan_validate_query_index().
				 */

				/* Check if the index covers our column */
				int nkeys = indexForm->indnatts;
				int i;

				for (i = 0; i < nkeys; i++)
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
	}

	systable_endscan(scan);
	table_close(indexRelation, AccessShareLock);

	/*
	 * Warn if multiple BM25 indexes exist on the same column.
	 * The planner will use the first one found, which may not be
	 * what the user intended.
	 */
	if (index_count > 1)
	{
		ereport(WARNING,
				(errmsg("multiple BM25 indexes exist on the same column"),
				 errhint("Use explicit to_bm25query('query', 'index_name') "
						 "to specify which index to use.")));
	}

	return result;
}

/*
 * Extract the relation OID and attribute number from a Var node,
 * handling range table entries.
 */
static bool
get_var_relation_and_attnum(
		Var *var, Query *query, Oid *relid, AttrNumber *attnum)
{
	RangeTblEntry *rte;

	if (var->varno < 1 || var->varno > list_length(query->rtable))
		return false;

	rte = rt_fetch(var->varno, query->rtable);

	/* We only handle simple relation references */
	if (rte->rtekind != RTE_RELATION)
		return false;

	*relid	= rte->relid;
	*attnum = var->varattno;

	return true;
}

/*
 * Create a new tpquery Const with the resolved index OID
 */
static Const *
create_resolved_tpquery_const(Const *original, Oid index_oid)
{
	TpQuery *old_tpquery;
	TpQuery *new_tpquery;
	char	*query_text;
	Const	*new_const;

	/* Get the original tpquery */
	old_tpquery = (TpQuery *)DatumGetPointer(original->constvalue);
	query_text	= get_tpquery_text(old_tpquery);

	/* Create new tpquery with resolved OID */
	new_tpquery = create_tpquery(query_text, index_oid);

	/* Create a new Const node */
	new_const = makeConst(
			original->consttype,
			original->consttypmod,
			original->constcollid,
			VARSIZE(new_tpquery),
			PointerGetDatum(new_tpquery),
			false,	/* constisnull */
			false); /* constbyval */

	return new_const;
}

/*
 * Mutator function to resolve unresolved tpquery constants
 */
static Node *
resolve_index_mutator(Node *node, ResolveIndexContext *context)
{
	BM25OidCache *oids = context->oid_cache;

	if (node == NULL)
		return NULL;

	/* Check for OpExpr with our <@> operator */
	if (IsA(node, OpExpr))
	{
		OpExpr *opexpr = (OpExpr *)node;

		if (opexpr->opno == oids->text_tpquery_operator_oid &&
			list_length(opexpr->args) == 2)
		{
			Node *left	= linitial(opexpr->args);
			Node *right = lsecond(opexpr->args);

			/*
			 * If the right arg is a function call (e.g., to_bm25query()),
			 * try to simplify it to a constant first.
			 */
			if (IsA(right, FuncExpr))
				right = eval_const_expressions(NULL, right);

			/* Check if right arg is a tpquery constant */
			if (IsA(right, Const))
			{
				Const *constNode = (Const *)right;

				if (constNode->consttype == oids->tpquery_type_oid &&
					!constNode->constisnull)
				{
					TpQuery *tpquery = (TpQuery *)DatumGetPointer(
							constNode->constvalue);

					/* Check if unresolved (InvalidOid) */
					if (!OidIsValid(tpquery->index_oid))
					{
						/* Try to find index from left operand */
						if (IsA(left, Var))
						{
							Var		  *var = (Var *)left;
							Oid		   relid;
							AttrNumber attnum;

							if (get_var_relation_and_attnum(
										var, context->query, &relid, &attnum))
							{
								Oid index_oid =
										find_bm25_index_for_column_internal(
												relid,
												attnum,
												oids->bm25_am_oid);

								if (OidIsValid(index_oid))
								{
									/* Create new OpExpr with resolved tpquery
									 */
									OpExpr *new_opexpr;
									Const  *new_const;

									new_const = create_resolved_tpquery_const(
											constNode, index_oid);

									new_opexpr			 = makeNode(OpExpr);
									new_opexpr->opno	 = opexpr->opno;
									new_opexpr->opfuncid = opexpr->opfuncid;
									new_opexpr->opresulttype =
											opexpr->opresulttype;
									new_opexpr->opretset = opexpr->opretset;
									new_opexpr->opcollid = opexpr->opcollid;
									new_opexpr->inputcollid =
											opexpr->inputcollid;
									new_opexpr->args = list_make2(
											copyObject(left), new_const);
									new_opexpr->location = opexpr->location;

									elog(DEBUG2,
										 "tp_planner: resolved tpquery "
										 "index_oid=%u",
										 index_oid);

									return (Node *)new_opexpr;
								}
							}
						}
					}
				}
			}
		}

		/*
		 * For text <@> text operators, transform to text <@> bm25query
		 * by resolving the BM25 index from the left operand's column.
		 * This enables standalone scoring in SELECT/WHERE clauses.
		 *
		 * The transformation enables text <@> text scoring to work
		 * everywhere (SELECT, WHERE, ORDER BY) by resolving the index
		 * automatically. The text <@> text operator is also registered
		 * in the opclass for ORDER BY, so Index Scan can be used.
		 */
		if (opexpr->opno == oids->text_text_operator_oid &&
			list_length(opexpr->args) == 2)
		{
			Node *left	= linitial(opexpr->args);
			Node *right = lsecond(opexpr->args);

			if (IsA(left, Var) && IsA(right, Const))
			{
				Var	  *var		  = (Var *)left;
				Const *text_const = (Const *)right;

				if (text_const->consttype == TEXTOID &&
					!text_const->constisnull)
				{
					Oid		   relid;
					AttrNumber attnum;

					if (get_var_relation_and_attnum(
								var, context->query, &relid, &attnum))
					{
						Oid index_oid = find_bm25_index_for_column_internal(
								relid, attnum, oids->bm25_am_oid);

						if (OidIsValid(index_oid))
						{
							/* Create transformed expression */
							OpExpr	*new_opexpr;
							Const	*tpquery_const;
							char	*query_text;
							TpQuery *tpquery;

							query_text = TextDatumGetCString(
									text_const->constvalue);
							tpquery = create_tpquery(query_text, index_oid);

							tpquery_const = makeConst(
									oids->tpquery_type_oid,
									-1,
									InvalidOid,
									VARSIZE(tpquery),
									PointerGetDatum(tpquery),
									false,
									false);

							new_opexpr		 = makeNode(OpExpr);
							new_opexpr->opno = oids->text_tpquery_operator_oid;
							new_opexpr->opfuncid = get_opcode(
									oids->text_tpquery_operator_oid);
							new_opexpr->opresulttype = FLOAT8OID;
							new_opexpr->opretset	 = false;
							new_opexpr->opcollid	 = InvalidOid;
							new_opexpr->inputcollid	 = InvalidOid;
							new_opexpr->args		 = list_make2(
									copyObject(left), tpquery_const);
							new_opexpr->location = opexpr->location;

							pfree(query_text);

							elog(DEBUG2,
								 "tp_planner: transformed text <@> text "
								 "to text <@> bm25query, index_oid=%u",
								 index_oid);

							return (Node *)new_opexpr;
						}
					}
				}
			}
		}
	}

	/* Recurse into child nodes */
	return expression_tree_mutator(node, resolve_index_mutator, context);
}

static void
resolve_indexes_in_query(Query *query)
{
	ResolveIndexContext context;
	BM25OidCache		oid_cache;

	/* Look up BM25 extension OIDs. If extension not installed, skip. */
	if (!get_bm25_oids(&oid_cache))
		return;

	context.query	  = query;
	context.oid_cache = &oid_cache;

	/* Process the target list (SELECT and ORDER BY expressions) */
	if (query->targetList)
	{
		ListCell *lc;

		foreach (lc, query->targetList)
		{
			TargetEntry *tle = (TargetEntry *)lfirst(lc);

			tle->expr = (Expr *)
					resolve_index_mutator((Node *)tle->expr, &context);
		}
	}

	/* Process WHERE clause */
	if (query->jointree && query->jointree->quals)
	{
		query->jointree->quals =
				resolve_index_mutator(query->jointree->quals, &context);
	}

	/* Process HAVING clause */
	if (query->havingQual)
	{
		query->havingQual = resolve_index_mutator(query->havingQual, &context);
	}

	/* Process CTEs (WITH clauses) - recursively resolve each CTE's query */
	if (query->cteList)
	{
		ListCell *lc;

		foreach (lc, query->cteList)
		{
			CommonTableExpr *cte = (CommonTableExpr *)lfirst(lc);

			if (cte->ctequery && IsA(cte->ctequery, Query))
			{
				resolve_indexes_in_query((Query *)cte->ctequery);
			}
		}
	}

	/* Process subqueries in FROM clause */
	if (query->rtable)
	{
		ListCell *lc;

		foreach (lc, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *)lfirst(lc);

			if (rte->rtekind == RTE_SUBQUERY && rte->subquery)
			{
				resolve_indexes_in_query(rte->subquery);
			}
		}
	}
}

/*
 * Main planner hook function
 */
PlannedStmt *
tp_planner_hook(
		Query		 *parse,
		const char	 *query_string,
		int			  cursorOptions,
		ParamListInfo boundParams)
{
	/* Try to resolve unresolved indexes before planning */
	resolve_indexes_in_query(parse);

	/* Call previous hook or standard planner */
	if (prev_planner_hook)
		return prev_planner_hook(
				parse, query_string, cursorOptions, boundParams);
	else
		return standard_planner(
				parse, query_string, cursorOptions, boundParams);
}

/*
 * Initialize planner hook (called from _PG_init)
 */
void
tp_planner_hook_init(void)
{
	elog(DEBUG2, "tp_planner_hook_init: installing planner hook");
	prev_planner_hook = planner_hook;
	planner_hook	  = tp_planner_hook;
}
