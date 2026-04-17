/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * resolve.c - Index name resolution and query validation
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/htup_details.h>
#include <access/table.h>
#include <catalog/namespace.h>
#include <catalog/pg_am.h>
#include <catalog/pg_index.h>
#include <catalog/pg_inherits.h>
#include <utils/builtins.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/regproc.h>
#include <utils/rel.h>
#include <utils/syscache.h>

#include "index/resolve.h"

/*
 * Maximum depth for walking inheritance hierarchies.
 * Prevents infinite loops in case of catalog corruption.
 */
#define MAX_INHERITANCE_DEPTH 32

/*
 * Get the appropriate index name for the given index relation.
 * Returns a qualified name (schema.index) if the index is not visible
 * in the search path, otherwise returns just the index name.
 */
char *
tp_get_qualified_index_name(Relation indexRelation)
{
	Oid index_namespace = RelationGetNamespace(indexRelation);

	/*
	 * If the index is not visible in the search path, use a qualified name
	 */
	if (!RelationIsVisible(RelationGetRelid(indexRelation)))
	{
		char *namespace_name = get_namespace_name(index_namespace);
		char *relation_name	 = RelationGetRelationName(indexRelation);
		return quote_qualified_identifier(namespace_name, relation_name);
	}
	else
	{
		return RelationGetRelationName(indexRelation);
	}
}

/*
 * Resolve index name to OID with schema support.
 * Returns the OID of the index, or InvalidOid if not found.
 * Handles both schema-qualified names (schema.index) and unqualified names.
 */
Oid
tp_resolve_index_name_shared(const char *index_name)
{
	Oid index_oid;

	if (strchr(index_name, '.') != NULL)
	{
		/* Contains a dot - try to parse as schema.relation */
		List *namelist = stringToQualifiedNameList(index_name, NULL);
		if (list_length(namelist) == 2)
		{
			char *schemaname = strVal(linitial(namelist));
			char *relname	 = strVal(lsecond(namelist));

			/* Validate that schema name is not empty */
			if (schemaname == NULL || strlen(schemaname) == 0)
			{
				index_oid = InvalidOid;
			}
			else
			{
				Oid namespace_oid = get_namespace_oid(schemaname, true);

				if (OidIsValid(namespace_oid))
					index_oid = get_relname_relid(relname, namespace_oid);
				else
					index_oid = InvalidOid;
			}
		}
		else
		{
			index_oid = InvalidOid;
		}
		list_free_deep(namelist);
	}
	else
	{
		/* No schema specified - use search path */
		index_oid = RelnameGetRelid(index_name);
	}

	return index_oid;
}

/*
 * Check if child_oid inherits from ancestor_oid via pg_inherits.
 * Walks up the inheritance chain to handle multi-level partitions.
 */
static bool
oid_inherits_from(Oid child_oid, Oid ancestor_oid)
{
	Relation inhrel;
	Oid		 current_oid = child_oid;
	bool	 found		 = false;
	int		 depth		 = MAX_INHERITANCE_DEPTH;

	if (child_oid == ancestor_oid)
		return true;

	inhrel = table_open(InheritsRelationId, AccessShareLock);

	while (depth-- > 0)
	{
		SysScanDesc scan;
		ScanKeyData key;
		HeapTuple	tuple;
		Oid			parent_oid = InvalidOid;

		ScanKeyInit(
				&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(current_oid));

		scan = systable_beginscan(
				inhrel, InheritsRelidSeqnoIndexId, true, NULL, 1, &key);

		tuple = systable_getnext(scan);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_inherits inhform = (Form_pg_inherits)GETSTRUCT(tuple);
			parent_oid				 = inhform->inhparent;
		}

		systable_endscan(scan);

		if (!OidIsValid(parent_oid))
			break; /* Reached top of hierarchy */

		if (parent_oid == ancestor_oid)
		{
			found = true;
			break;
		}

		current_oid = parent_oid;
	}

	table_close(inhrel, AccessShareLock);

	return found;
}

/*
 * Check if two BM25 indexes match by attribute (for hypertables).
 *
 * This handles cases where chunk indexes don't have pg_inherits relationships
 * to the parent index (e.g., TimescaleDB hypertables). Instead we check:
 * 1. Both indexes use the BM25 access method
 * 2. The scan index's table inherits from the query index's table
 * 3. Both indexes are on the same column attribute number
 */
static bool
indexes_match_by_attribute(Oid scan_index_oid, Oid query_index_oid)
{
	HeapTuple  scan_idx_tuple;
	HeapTuple  query_idx_tuple;
	HeapTuple  scan_class_tuple;
	HeapTuple  query_class_tuple;
	Oid		   scan_heap_oid;
	Oid		   query_heap_oid;
	Oid		   bm25_am_oid;
	bool	   result = false;
	AttrNumber scan_attnum;
	AttrNumber query_attnum;
	HeapTuple  am_tuple;

	/* Look up bm25 access method OID */
	am_tuple = SearchSysCache1(AMNAME, CStringGetDatum("bm25"));
	if (!HeapTupleIsValid(am_tuple))
		return false;
	bm25_am_oid = ((Form_pg_am)GETSTRUCT(am_tuple))->oid;
	ReleaseSysCache(am_tuple);

	/* Get pg_index entries for both indexes */
	scan_idx_tuple =
			SearchSysCache1(INDEXRELID, ObjectIdGetDatum(scan_index_oid));
	if (!HeapTupleIsValid(scan_idx_tuple))
		return false;

	query_idx_tuple =
			SearchSysCache1(INDEXRELID, ObjectIdGetDatum(query_index_oid));
	if (!HeapTupleIsValid(query_idx_tuple))
	{
		ReleaseSysCache(scan_idx_tuple);
		return false;
	}

	/* Get heap OIDs from pg_index */
	scan_heap_oid  = ((Form_pg_index)GETSTRUCT(scan_idx_tuple))->indrelid;
	query_heap_oid = ((Form_pg_index)GETSTRUCT(query_idx_tuple))->indrelid;

	/* Get attribute numbers (assume single-column BM25 indexes) */
	scan_attnum = ((Form_pg_index)GETSTRUCT(scan_idx_tuple))->indkey.values[0];
	query_attnum =
			((Form_pg_index)GETSTRUCT(query_idx_tuple))->indkey.values[0];

	/* Check if both indexes use BM25 access method */
	scan_class_tuple =
			SearchSysCache1(RELOID, ObjectIdGetDatum(scan_index_oid));
	query_class_tuple =
			SearchSysCache1(RELOID, ObjectIdGetDatum(query_index_oid));

	if (HeapTupleIsValid(scan_class_tuple) &&
		HeapTupleIsValid(query_class_tuple))
	{
		Oid scan_am	 = ((Form_pg_class)GETSTRUCT(scan_class_tuple))->relam;
		Oid query_am = ((Form_pg_class)GETSTRUCT(query_class_tuple))->relam;

		if (scan_am == bm25_am_oid && query_am == bm25_am_oid &&
			oid_inherits_from(scan_heap_oid, query_heap_oid))
		{
			if (scan_attnum != 0 && query_attnum != 0)
			{
				/*
				 * Plain column: compare by column name rather
				 * than raw attnum.  Dropped columns can cause
				 * parent and child tables to have different
				 * physical attnums for the same logical column
				 * (e.g., TimescaleDB hypertables or inheritance
				 * after ALTER TABLE DROP COLUMN).
				 */
				char *scan_colname =
						get_attname(scan_heap_oid, scan_attnum, true);
				char *query_colname =
						get_attname(query_heap_oid, query_attnum, true);

				if (scan_colname && query_colname &&
					strcmp(scan_colname, query_colname) == 0)
				{
					result = true;
				}
			}
			else if (scan_attnum == 0 && query_attnum == 0)
			{
				/*
				 * Expression indexes: compare stored
				 * expression trees from pg_index.
				 */
				Datum scan_expr_d, query_expr_d;
				bool  scan_null, query_null;

				scan_expr_d = SysCacheGetAttr(
						INDEXRELID,
						scan_idx_tuple,
						Anum_pg_index_indexprs,
						&scan_null);
				query_expr_d = SysCacheGetAttr(
						INDEXRELID,
						query_idx_tuple,
						Anum_pg_index_indexprs,
						&query_null);

				if (!scan_null && !query_null)
				{
					char *scan_str	  = TextDatumGetCString(scan_expr_d);
					char *query_str	  = TextDatumGetCString(query_expr_d);
					List *scan_exprs  = (List *)stringToNode(scan_str);
					List *query_exprs = (List *)stringToNode(query_str);

					pfree(scan_str);
					pfree(query_str);

					if (equal(scan_exprs, query_exprs))
						result = true;
				}
			}
		}
	}

	/* Cleanup */
	if (HeapTupleIsValid(scan_class_tuple))
		ReleaseSysCache(scan_class_tuple);
	if (HeapTupleIsValid(query_class_tuple))
		ReleaseSysCache(query_class_tuple);
	ReleaseSysCache(scan_idx_tuple);
	ReleaseSysCache(query_idx_tuple);

	return result;
}

/*
 * Validate that the query index OID matches the scan index.
 * Allows partitioned index queries to run on partition indexes.
 */
void
tp_validate_query_index(Oid query_index_oid, Relation indexRelation)
{
	Oid scan_index_oid = RelationGetRelid(indexRelation);

	/* Direct match - OK */
	if (query_index_oid == scan_index_oid)
		return;

	/*
	 * Check if query references a partitioned index and scan is on a
	 * partition index (child of the partitioned index).
	 */
	if (get_rel_relkind(query_index_oid) == RELKIND_PARTITIONED_INDEX &&
		oid_inherits_from(scan_index_oid, query_index_oid))
		return;

	/*
	 * Attribute-based matching for TimescaleDB hypertables and other cases
	 * where chunk indexes don't have pg_inherits relationships to the parent.
	 */
	if (indexes_match_by_attribute(scan_index_oid, query_index_oid))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("tpquery index mismatch"),
			 errhint("Query specifies index OID %u but scan is on "
					 "index \"%s\" (OID %u)",
					 query_index_oid,
					 RelationGetRelationName(indexRelation),
					 scan_index_oid)));
}
