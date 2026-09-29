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
#include <catalog/pg_class.h>
#include <catalog/pg_index.h>
#include <catalog/pg_inherits.h>
#include <commands/defrem.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/relcache.h>
#include <utils/snapmgr.h>
#include <utils/syscache.h>

#include "access/rls.h"
#include "constants.h"

static bool
relation_has_rls(Oid relid)
{
	HeapTuple tuple;
	bool	  has_rls;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return false;

	has_rls = ((Form_pg_class)GETSTRUCT(tuple))->relrowsecurity;
	ReleaseSysCache(tuple);
	return has_rls;
}

static bool
relation_has_bm25_index(Oid relid)
{
	Relation	indexrel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			bm25_am_oid;
	bool		found = false;

	bm25_am_oid = get_am_oid("bm25", true);
	if (!OidIsValid(bm25_am_oid))
		return false;

	indexrel = table_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(
			&key,
			Anum_pg_index_indrelid,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(relid));
	scan = systable_beginscan(
			indexrel, IndexIndrelidIndexId, true, SnapshotSelf, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_index indexform = (Form_pg_index)GETSTRUCT(tuple);
		HeapTuple	  classtuple;

		if (!indexform->indislive)
			continue;

		classtuple = SearchSysCache1(
				RELOID, ObjectIdGetDatum(indexform->indexrelid));
		if (HeapTupleIsValid(classtuple))
		{
			found = ((Form_pg_class)GETSTRUCT(classtuple))->relam ==
					bm25_am_oid;
			ReleaseSysCache(classtuple);
		}
		if (found)
			break;
	}

	systable_endscan(scan);
	table_close(indexrel, AccessShareLock);
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
		ScanKeyData key;
		SysScanDesc scan;
		HeapTuple	tuple;

		pending = list_delete_first(pending);
		if (list_member_oid(visited, current))
			continue;
		visited = lappend_oid(visited, current);

		if (relation_has_rls(current))
		{
			result = current;
			break;
		}

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

	relations = find_all_inheritors(relid, NoLock, NULL);
	foreach (lc, relations)
	{
		Oid current = lfirst_oid(lc);

		if (relation_has_bm25_index(current))
			result = current;

		if (OidIsValid(result))
			break;
	}

	list_free(relations);
	return result;
}

static Oid
find_index_heap_relation(Oid indexrelid)
{
	Relation	indexrel;
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			heaprelid = InvalidOid;

	indexrel = table_open(IndexRelationId, AccessShareLock);
	ScanKeyInit(
			&key,
			Anum_pg_index_indexrelid,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(indexrelid));
	scan = systable_beginscan(
			indexrel, IndexRelidIndexId, true, SnapshotSelf, 1, &key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		heaprelid = ((Form_pg_index)GETSTRUCT(tuple))->indrelid;
	systable_endscan(scan);
	table_close(indexrel, AccessShareLock);

	return heaprelid;
}

static void
check_bm25_build_allowed(Oid heaprelid)
{
	Oid rls_relid;

	if (tp_rls_allowed_for_current_utility())
		return;

	rls_relid = find_rls_ancestor(heaprelid);
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
					 get_rel_name(heaprelid)),
			 errhint("Set pg_textsearch.allow_rls to on to accept the "
					 "documented term-frequency leakage risk.")));
}

void
tp_check_bm25_build_allowed(Relation heap)
{
	check_bm25_build_allowed(RelationGetRelid(heap));
}

bool
tp_check_bm25_index_create_allowed(Oid indexrelid)
{
	Oid		 bm25_am_oid;
	Oid		 heaprelid;
	Relation index;
	Relation heap;
	bool	 is_bm25;

	bm25_am_oid = get_am_oid("bm25", true);
	if (!OidIsValid(bm25_am_oid))
		return false;

	index	= relation_open(indexrelid, NoLock);
	is_bm25 = (index->rd_rel->relkind == RELKIND_INDEX ||
			   index->rd_rel->relkind == RELKIND_PARTITIONED_INDEX) &&
			  index->rd_rel->relam == bm25_am_oid;
	relation_close(index, NoLock);

	if (!is_bm25)
		return false;

	heaprelid = find_index_heap_relation(indexrelid);
	if (!OidIsValid(heaprelid))
		elog(ERROR,
			 "could not find heap relation for newly created BM25 index %u",
			 indexrelid);

	heap = table_open(heaprelid, NoLock);
	tp_check_bm25_build_allowed(heap);
	table_close(heap, NoLock);

	return true;
}

void
tp_check_bm25_hierarchy_allowed(Oid relid)
{
	List	 *relations;
	ListCell *lc;

	if (tp_rls_allowed_for_current_utility())
		return;

	relations = find_all_inheritors(relid, NoLock, NULL);
	foreach (lc, relations)
	{
		Oid current = lfirst_oid(lc);

		if (relation_has_bm25_index(current))
			check_bm25_build_allowed(current);
	}

	list_free(relations);
}

void
tp_check_rls_enable_allowed(Oid relid)
{
	Oid indexed_relid;

	if (tp_rls_allowed_for_current_utility())
		return;

	if (!relation_has_rls(relid))
	{
		return;
	}

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
