/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * rls.c - BM25 and row-level security policy checks
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/heapam.h>
#include <access/relation.h>
#include <access/skey.h>
#include <access/table.h>
#include <catalog/indexing.h>
#include <catalog/pg_inherits.h>
#include <commands/defrem.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/relcache.h>

#include "access/rls.h"
#include "constants.h"

static bool
relation_has_bm25_index(Relation rel)
{
	List	 *indexes;
	ListCell *lc;
	Oid		  bm25_am_oid;
	bool	  found = false;

	bm25_am_oid = get_am_oid("bm25", true);
	if (!OidIsValid(bm25_am_oid))
		return false;

	indexes = RelationGetIndexList(rel);

	foreach (lc, indexes)
	{
		Relation index = index_open(lfirst_oid(lc), AccessShareLock);

		if (index->rd_rel->relam == bm25_am_oid)
			found = true;

		index_close(index, AccessShareLock);
		if (found)
			break;
	}

	list_free(indexes);
	return found;
}

static Oid
find_rls_ancestor(Oid relid)
{
	Relation inhrel;
	List	*pending = list_make1_oid(relid);
	List	*visited = NIL;
	Oid		 result	 = InvalidOid;

	inhrel = table_open(InheritsRelationId, AccessShareLock);

	while (pending != NIL)
	{
		Oid			current = linitial_oid(pending);
		Relation	rel;
		ScanKeyData key;
		SysScanDesc scan;
		HeapTuple	tuple;

		pending = list_delete_first(pending);
		if (list_member_oid(visited, current))
			continue;
		visited = lappend_oid(visited, current);

		rel = table_open(current, AccessShareLock);
		if (rel->rd_rel->relrowsecurity)
		{
			result = current;
			table_close(rel, AccessShareLock);
			break;
		}
		table_close(rel, AccessShareLock);

		ScanKeyInit(
				&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(current));
		scan = systable_beginscan(
				inhrel, InheritsRelidSeqnoIndexId, true, NULL, 1, &key);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			Form_pg_inherits inhform = (Form_pg_inherits)GETSTRUCT(tuple);

			pending = lappend_oid(pending, inhform->inhparent);
		}
		systable_endscan(scan);
	}

	table_close(inhrel, AccessShareLock);
	list_free(pending);
	list_free(visited);
	return result;
}

static Oid
find_bm25_indexed_relation(Oid relid)
{
	List	 *relations;
	ListCell *lc;
	Oid		  result = InvalidOid;

	relations = find_all_inheritors(relid, AccessShareLock, NULL);
	foreach (lc, relations)
	{
		Oid		 current = lfirst_oid(lc);
		Relation rel	 = table_open(current, NoLock);

		if (relation_has_bm25_index(rel))
			result = current;

		table_close(rel, NoLock);
		if (OidIsValid(result))
			break;
	}

	list_free(relations);
	return result;
}

void
tp_check_bm25_build_allowed(Relation heap)
{
	Oid rls_relid;

	if (tp_allow_rls)
		return;

	rls_relid = find_rls_ancestor(RelationGetRelid(heap));
	if (!OidIsValid(rls_relid))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("BM25 indexes are not allowed on row-level security "
					"relation \"%s\" when "
					"\"pg_textsearch.allow_rls\" is off",
					get_rel_name(rls_relid)),
			 errdetail(
					 "Relation \"%s\" is in the protected relation's "
					 "inheritance hierarchy.",
					 RelationGetRelationName(heap)),
			 errhint("Set pg_textsearch.allow_rls to on to accept the "
					 "documented term-frequency leakage risk.")));
}

void
tp_check_rls_enable_allowed(Oid relid)
{
	Oid		 indexed_relid;
	Relation rel;

	if (tp_allow_rls)
		return;

	rel = table_open(relid, NoLock);
	if (!rel->rd_rel->relrowsecurity)
	{
		table_close(rel, NoLock);
		return;
	}
	table_close(rel, NoLock);

	indexed_relid = find_bm25_indexed_relation(relid);
	if (!OidIsValid(indexed_relid))
		return;

	if (indexed_relid == relid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot enable row-level security on relation \"%s\" "
						"because it has a BM25 index while "
						"\"pg_textsearch.allow_rls\" is off",
						get_rel_name(relid)),
				 errhint("Set pg_textsearch.allow_rls to on to accept the "
						 "documented term-frequency leakage risk.")));

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot enable row-level security on relation \"%s\" "
					"because descendant relation \"%s\" has a BM25 index "
					"while \"pg_textsearch.allow_rls\" is off",
					get_rel_name(relid),
					get_rel_name(indexed_relid)),
			 errhint("Set pg_textsearch.allow_rls to on to accept the "
					 "documented term-frequency leakage risk.")));
}
