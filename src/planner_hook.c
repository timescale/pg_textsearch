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
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <optimizer/optimizer.h>
#include <optimizer/planner.h>
#include <parser/parse_oper.h>
#include <parser/parsetree.h>
#include <utils/builtins.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/syscache.h>

#include "planner_hook.h"
#include "query.h"

/* Previous planner hook in chain */
static planner_hook_type prev_planner_hook = NULL;

/* Cached OIDs (lazily initialized) */
static Oid bm25_am_oid				 = InvalidOid;
static Oid tpquery_type_oid			 = InvalidOid;
static Oid text_tpquery_operator_oid = InvalidOid;
static Oid text_text_operator_oid	 = InvalidOid;

/*
 * Context for query tree mutation
 */
typedef struct ResolveIndexContext
{
	Query *query; /* The query being processed */
} ResolveIndexContext;

/*
 * Initialize cached OIDs
 */
static void
init_cached_oids(void)
{
	if (!OidIsValid(bm25_am_oid))
	{
		HeapTuple tuple = SearchSysCache1(AMNAME, CStringGetDatum("bm25"));
		if (HeapTupleIsValid(tuple))
		{
			bm25_am_oid = ((Form_pg_am)GETSTRUCT(tuple))->oid;
			ReleaseSysCache(tuple);
		}
		elog(DEBUG1, "tp_planner_hook: bm25_am_oid = %u", bm25_am_oid);
	}

	if (!OidIsValid(tpquery_type_oid))
	{
		/*
		 * TypenameGetTypid searches the current search_path, which may not
		 * include the schema where bm25query is defined. Instead, look up
		 * the type in the public schema (where our extension creates it).
		 */
		Oid namespace_oid = get_namespace_oid("public", true);

		if (OidIsValid(namespace_oid))
		{
			tpquery_type_oid = GetSysCacheOid2(
					TYPENAMENSP,
					Anum_pg_type_oid,
					CStringGetDatum("bm25query"),
					ObjectIdGetDatum(namespace_oid));
		}

		elog(DEBUG1,
			 "tp_planner_hook: tpquery_type_oid = %u",
			 tpquery_type_oid);
	}

	if (!OidIsValid(text_tpquery_operator_oid))
	{
		/* Look up the <@> operator for (text, bm25query) */
		List *opname = list_make1(makeString("<@>"));

		text_tpquery_operator_oid =
				OpernameGetOprid(opname, TEXTOID, tpquery_type_oid);
		list_free(opname);
		elog(DEBUG1,
			 "tp_planner_hook: text_tpquery_operator_oid = %u",
			 text_tpquery_operator_oid);
	}

	if (!OidIsValid(text_text_operator_oid))
	{
		/* Look up the <@> operator for (text, text) */
		List *opname = list_make1(makeString("<@>"));

		text_text_operator_oid = OpernameGetOprid(opname, TEXTOID, TEXTOID);
		list_free(opname);
		elog(DEBUG1,
			 "tp_planner_hook: text_text_operator_oid = %u",
			 text_text_operator_oid);
	}
}

/*
 * Find a BM25 index on a specific column of a relation
 *
 * Returns the index OID, or InvalidOid if no suitable index found.
 *
 * Note: This function skips partitioned indexes (RELKIND_PARTITIONED_INDEX)
 * because they don't have storage and can't be used directly. For partitioned
 * tables, the user must query each partition explicitly or future versions
 * will support partition-aware index scans.
 */
static Oid
find_bm25_index_for_column(Oid relid, AttrNumber attnum)
{
	Relation	indexRelation;
	SysScanDesc scan;
	HeapTuple	indexTuple;
	Oid			result = InvalidOid;
	ScanKeyData scanKey;

	/* Initialize cached OIDs if needed */
	init_cached_oids();

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
				 * to partition indexes at scan time via is_partition_of()
				 * in tp_rescan_validate_query_index().
				 */

				/* Check if the index covers our column */
				int nkeys = indexForm->indnatts;
				int i;

				for (i = 0; i < nkeys; i++)
				{
					if (indexForm->indkey.values[i] == attnum)
					{
						result = indexOid;
						ReleaseSysCache(classTuple);
						goto done;
					}
				}
			}
			ReleaseSysCache(classTuple);
		}
	}

done:
	systable_endscan(scan);
	table_close(indexRelation, AccessShareLock);

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
	if (node == NULL)
		return NULL;

	/* Check for OpExpr with our <@> operator */
	if (IsA(node, OpExpr))
	{
		OpExpr *opexpr = (OpExpr *)node;

		init_cached_oids();

		elog(DEBUG1,
			 "tp_planner_hook: found OpExpr opno=%u, looking for=%u",
			 opexpr->opno,
			 text_tpquery_operator_oid);

		if (opexpr->opno == text_tpquery_operator_oid &&
			list_length(opexpr->args) == 2)
		{
			Node *left	= linitial(opexpr->args);
			Node *right = lsecond(opexpr->args);

			elog(DEBUG1,
				 "tp_planner_hook: matched <@> operator, left=%d "
				 "right=%d",
				 nodeTag(left),
				 nodeTag(right));

			/*
			 * If the right arg is a function call (e.g., to_bm25query()),
			 * try to simplify it to a constant first.
			 */
			if (IsA(right, FuncExpr))
			{
				elog(DEBUG1,
					 "tp_planner_hook: right is FuncExpr, trying "
					 "eval_const_expressions");
				right = eval_const_expressions(NULL, right);
				elog(DEBUG1,
					 "tp_planner_hook: after eval, right=%d",
					 nodeTag(right));
			}

			/* Check if right arg is a tpquery constant */
			if (IsA(right, Const))
			{
				Const *constNode = (Const *)right;

				elog(DEBUG1,
					 "tp_planner_hook: Const type=%u, expected=%u, "
					 "isnull=%d",
					 constNode->consttype,
					 tpquery_type_oid,
					 constNode->constisnull);

				if (constNode->consttype == tpquery_type_oid &&
					!constNode->constisnull)
				{
					TpQuery *tpquery = (TpQuery *)DatumGetPointer(
							constNode->constvalue);

					elog(DEBUG1,
						 "tp_planner_hook: tpquery index_oid=%u",
						 tpquery->index_oid);

					/* Check if unresolved (InvalidOid) */
					if (!OidIsValid(tpquery->index_oid))
					{
						elog(DEBUG1,
							 "tp_planner_hook: index unresolved, "
							 "trying to find from Var");

						/* Try to find index from left operand */
						if (IsA(left, Var))
						{
							Var		  *var = (Var *)left;
							Oid		   relid;
							AttrNumber attnum;

							if (get_var_relation_and_attnum(
										var, context->query, &relid, &attnum))
							{
								Oid index_oid;

								elog(DEBUG1,
									 "tp_planner_hook: looking for "
									 "bm25 index on rel=%u col=%d",
									 relid,
									 attnum);

								index_oid = find_bm25_index_for_column(
										relid, attnum);

								elog(DEBUG1,
									 "tp_planner_hook: found "
									 "index_oid=%u",
									 index_oid);

								if (OidIsValid(index_oid))
								{
									/* Create new OpExpr with resolved tpquery
									 */
									OpExpr *new_opexpr;
									Const  *new_const;

									elog(DEBUG1,
										 "tp_planner_hook: creating "
										 "resolved tpquery with oid=%u",
										 index_oid);

									new_const = create_resolved_tpquery_const(
											constNode, index_oid);

									new_opexpr = makeNode(OpExpr);
									memcpy(new_opexpr, opexpr, sizeof(OpExpr));
									new_opexpr->args = list_make2(
											copyObject(left), new_const);

									return (Node *)new_opexpr;
								}
							}
							else
							{
								elog(DEBUG1,
									 "tp_planner_hook: could not get "
									 "relation/attnum from Var");
							}
						}
						else
						{
							elog(DEBUG1,
								 "tp_planner_hook: left is not a Var "
								 "(type=%d)",
								 nodeTag(left));
						}
					}
					else
					{
						elog(DEBUG1,
							 "tp_planner_hook: index already resolved");
					}
				}
			}
		}

		/*
		 * Note: text <@> text operators are NOT transformed here.
		 * They are handled directly by the opclass and index AM.
		 * Transforming them would break pathkey matching for index ordering.
		 */
	}

	/* Recurse into child nodes */
	return expression_tree_mutator(node, resolve_index_mutator, context);
}

/*
 * Walk the query tree and resolve unresolved tpquery indexes
 */
static void
resolve_indexes_in_query(Query *query)
{
	ResolveIndexContext context;

	context.query = query;

	/* Process the target list (SELECT expressions) */
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

	/* Process ORDER BY clauses */
	if (query->sortClause)
	{
		/* Sort clause references target list entries, already processed */
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
	elog(DEBUG1,
		 "tp_planner_hook: entering for query type %d",
		 parse->commandType);

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
	elog(DEBUG1, "tp_planner_hook_init: installing planner hook");
	prev_planner_hook = planner_hook;
	planner_hook	  = tp_planner_hook;
}
