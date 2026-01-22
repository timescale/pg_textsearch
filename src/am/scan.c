/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am/scan.c - BM25 index scan operations
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/relscan.h>
#include <access/sdir.h>
#include <access/table.h>
#include <catalog/namespace.h>
#include <catalog/pg_am.h>
#include <catalog/pg_index.h>
#include <catalog/pg_inherits.h>
#include <parser/parse_type.h>
#include <parser/scansup.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/regproc.h>
#include <utils/rel.h>
#include <utils/syscache.h>

#include "am.h"
#include "constants.h"
#include "memtable/scan.h"
#include "state/limit.h"
#include "state/metapage.h"
#include "state/state.h"
#include "types/query.h"
#include "types/vector.h"

/*
 * Backend-local cached score for ORDER BY optimization.
 *
 * When tp_gettuple returns a row, the BM25 score is cached here. The
 * bm25_get_current_score() stub function returns this value, avoiding
 * re-computation of scores in resjunk ORDER BY expressions.
 */
static float8 tp_cached_score = 0.0;

float8
tp_get_cached_score(void)
{
	return tp_cached_score;
}

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
		return psprintf("%s.%s", namespace_name, relation_name);
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
 * Clean up any previous scan results in the scan opaque structure
 */
static void
tp_rescan_cleanup_results(TpScanOpaque so)
{
	if (!so)
		return;

	/* Clean up result CTIDs */
	if (so->result_ctids)
	{
		if (so->scan_context)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);
			pfree(so->result_ctids);
			so->result_ctids = NULL;
			MemoryContextSwitchTo(oldcontext);
		}
		else
		{
			elog(WARNING,
				 "No scan context available for cleanup - memory leak!");
			so->result_ctids = NULL;
		}
	}

	/* Clean up result scores */
	if (so->result_scores)
	{
		if (so->scan_context)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);
			pfree(so->result_scores);
			so->result_scores = NULL;
			MemoryContextSwitchTo(oldcontext);
		}
		else
		{
			elog(WARNING,
				 "No scan context available for cleanup - memory leak!");
			so->result_scores = NULL;
		}
	}
}

/*
 * Maximum depth for walking inheritance hierarchies.
 * Prevents infinite loops in case of catalog corruption.
 */
#define MAX_INHERITANCE_DEPTH 32

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
			scan_attnum == query_attnum &&
			oid_inherits_from(scan_heap_oid, query_heap_oid))
		{
			result = true;
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
static void
tp_rescan_validate_query_index(Oid query_index_oid, Relation indexRelation)
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

/*
 * Process ORDER BY scan keys for <@> operator
 *
 * Handles both bm25query and plain text arguments to support:
 * - ORDER BY content <@> 'query'::bm25query (explicit bm25query)
 * - ORDER BY content <@> 'query' (plain text, implicit index resolution)
 */
static void
tp_rescan_process_orderby(
		IndexScanDesc	scan,
		ScanKey			orderbys,
		int				norderbys,
		TpIndexMetaPage metap)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;

	for (int i = 0; i < norderbys; i++)
	{
		ScanKey orderby = &orderbys[i];

		/* Check for <@> operator strategy */
		if (orderby->sk_strategy == 1) /* Strategy 1: <@> operator */
		{
			Datum query_datum = orderby->sk_argument;
			char *query_cstr;
			Oid	  query_index_oid = InvalidOid;

			/*
			 * Use sk_subtype to determine the argument type.
			 * sk_subtype contains the right-hand operand's type OID.
			 */
			if (orderby->sk_subtype == TEXTOID)
			{
				/* Plain text - use text directly */
				text *query_text = (text *)DatumGetPointer(query_datum);

				query_cstr = text_to_cstring(query_text);
			}
			else
			{
				/* bm25query - extract query text and index OID */
				TpQuery *query = (TpQuery *)DatumGetPointer(query_datum);

				query_cstr		= pstrdup(get_tpquery_text(query));
				query_index_oid = get_tpquery_index_oid(query);

				/* Validate index OID if provided in query */
				if (tpquery_has_index(query))
				{
					tp_rescan_validate_query_index(
							query_index_oid, scan->indexRelation);
				}
			}

			/* Clear query vector since we're using text directly */
			if (so->query_vector)
			{
				pfree(so->query_vector);
				so->query_vector = NULL;
			}

			/* Free old query text if it exists */
			if (so->query_text)
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(
						so->scan_context);
				pfree(so->query_text);
				MemoryContextSwitchTo(oldcontext);
			}

			/* Allocate new query text in scan context */
			{
				MemoryContext oldcontext = MemoryContextSwitchTo(
						so->scan_context);
				so->query_text = pstrdup(query_cstr);
				MemoryContextSwitchTo(oldcontext);
			}

			/* Store index OID for this scan */
			so->index_oid = RelationGetRelid(scan->indexRelation);

			/* Mark all docs as candidates for ORDER BY operation */
			if (metap && metap->total_docs > 0)
				so->result_count = metap->total_docs;

			pfree(query_cstr);
		}
	}
}

/*
 * Begin a scan of the Tapir index
 */
IndexScanDesc
tp_beginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	TpScanOpaque  so;

	scan = RelationGetIndexScan(index, nkeys, norderbys);

	/* Allocate and initialize scan opaque data */
	so				 = (TpScanOpaque)palloc0(sizeof(TpScanOpaqueData));
	so->scan_context = AllocSetContextCreate(
			CurrentMemoryContext,
			"Tapir Scan Context",
			ALLOCSET_DEFAULT_SIZES);
	so->limit	 = -1; /* Initialize limit to -1 (no limit) */
	scan->opaque = so;

	/*
	 * Custom index AMs must allocate ORDER BY arrays themselves.
	 */
	if (norderbys > 0)
	{
		scan->xs_orderbyvals  = (Datum *)palloc0(sizeof(Datum) * norderbys);
		scan->xs_orderbynulls = (bool *)palloc(sizeof(bool) * norderbys);
		/* Initialize all orderbynulls to true */
		memset(scan->xs_orderbynulls, true, sizeof(bool) * norderbys);
	}

	return scan;
}

/*
 * Restart a scan with new keys
 */
void
tp_rescan(
		IndexScanDesc scan,
		ScanKey		  keys __attribute__((unused)),
		int			  nkeys __attribute__((unused)),
		ScanKey		  orderbys,
		int			  norderbys)
{
	TpScanOpaque	so	  = (TpScanOpaque)scan->opaque;
	TpIndexMetaPage metap = NULL;

	Assert(scan != NULL);
	Assert(scan->opaque != NULL);

	if (!so)
		return;

	/* Retrieve query LIMIT, if available */
	{
		int query_limit = tp_get_query_limit(scan->indexRelation);
		so->limit		= (query_limit > 0) ? query_limit : -1;
	}

	/* Reset scan state */
	if (so)
	{
		/* Clean up any previous results */
		tp_rescan_cleanup_results(so);

		/* Reset scan position and state */
		so->current_pos	 = 0;
		so->result_count = 0;
		so->eof_reached	 = false;
		so->query_vector = NULL;
	}

	/* Process ORDER BY scan keys for <@> operator */
	if (norderbys > 0 && orderbys && so)
	{
		/* Get index metadata to check if we have documents */
		if (!metap)
			metap = tp_get_metapage(scan->indexRelation);

		tp_rescan_process_orderby(scan, orderbys, norderbys, metap);

		if (metap)
			pfree(metap);
	}
}

/*
 * End a scan and cleanup resources
 */
void
tp_endscan(IndexScanDesc scan)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;

	if (so)
	{
		if (so->scan_context)
			MemoryContextDelete(so->scan_context);

		/* Free query vector if it was allocated */
		if (so->query_vector)
			pfree(so->query_vector);

		pfree(so);
		scan->opaque = NULL;
	}

	/*
	 * Don't free ORDER BY arrays here - PostgreSQL's core code will free them.
	 */
	if (scan->numberOfOrderBys > 0)
	{
		scan->xs_orderbyvals  = NULL;
		scan->xs_orderbynulls = NULL;
	}
}

/*
 * Execute BM25 scoring query to get ordered results
 */
static bool
tp_execute_scoring_query(IndexScanDesc scan)
{
	TpScanOpaque	   so = (TpScanOpaque)scan->opaque;
	TpIndexMetaPage	   metap;
	bool			   success	   = false;
	TpLocalIndexState *index_state = NULL;
	TpVector		  *query_vector;

	if (!so || !so->query_text)
		return false;

	Assert(so->scan_context != NULL);

	/* Clean up previous results */
	if (so->result_ctids || so->result_scores)
	{
		MemoryContext oldcontext = MemoryContextSwitchTo(so->scan_context);

		if (so->result_ctids)
		{
			pfree(so->result_ctids);
			so->result_ctids = NULL;
		}
		if (so->result_scores)
		{
			pfree(so->result_scores);
			so->result_scores = NULL;
		}

		MemoryContextSwitchTo(oldcontext);
	}

	so->result_count = 0;
	so->current_pos	 = 0;

	/* Get index metadata */
	metap = tp_get_metapage(scan->indexRelation);
	if (!metap)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to get metapage for index %s",
						RelationGetRelationName(scan->indexRelation))));
	}

	/* Get the index state with posting lists */
	index_state = tp_get_local_index_state(
			RelationGetRelid(scan->indexRelation));

	if (!index_state)
	{
		pfree(metap);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for BM25 search")));
	}

	/* Acquire shared lock for reading the memtable */
	tp_acquire_index_lock(index_state, LW_SHARED);

	/* Use the original query vector or create one from text */
	query_vector = so->query_vector;

	if (!query_vector && so->query_text)
	{
		/*
		 * We have a text query - convert it to a vector using the index.
		 */
		char *index_name = tp_get_qualified_index_name(scan->indexRelation);

		text *index_name_text  = cstring_to_text(index_name);
		text *query_text_datum = cstring_to_text(so->query_text);

		Datum query_vec_datum = DirectFunctionCall2(
				to_tpvector,
				PointerGetDatum(query_text_datum),
				PointerGetDatum(index_name_text));

		query_vector = (TpVector *)DatumGetPointer(query_vec_datum);

		/* Free existing query vector if present */
		if (so->query_vector)
			pfree(so->query_vector);

		/* Store the converted vector for this query execution */
		so->query_vector = query_vector;
	}

	if (!query_vector)
	{
		pfree(metap);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("no query vector available in scan state")));
	}

	/* Find documents matching the query using posting lists */
	success = tp_memtable_search(scan, index_state, query_vector, metap);

	/* Release the lock - we have extracted all CTIDs we need */
	tp_release_index_lock(index_state);

	pfree(metap);
	return success;
}

/*
 * Get next tuple from scan
 */
bool
tp_gettuple(IndexScanDesc scan, ScanDirection dir)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;
	float4		 bm25_score;
	BlockNumber	 blknum;

	(void)dir; /* BM25 index only supports forward scan */

	Assert(scan != NULL);
	Assert(so != NULL);
	Assert(so->query_text != NULL);

	/* Execute scoring query if we haven't done so yet */
	if (so->result_ctids == NULL && !so->eof_reached)
	{
		if (!tp_execute_scoring_query(scan))
		{
			so->eof_reached = true;
			return false;
		}
		/* Scoring query must have allocated result_ctids on success */
		if (so->result_ctids == NULL)
		{
			so->eof_reached = true;
			return false;
		}
	}

	/* Check if we've reached the end */
	if (so->current_pos >= so->result_count || so->eof_reached)
		return false;

	Assert(so->scan_context != NULL);
	Assert(so->result_ctids != NULL);
	Assert(so->current_pos < so->result_count);

	Assert(ItemPointerIsValid(&so->result_ctids[so->current_pos]));

	/* Additional validation - check for obviously invalid block numbers */
	blknum = BlockIdGetBlockNumber(
			&(so->result_ctids[so->current_pos].ip_blkid));
	if (blknum == InvalidBlockNumber || blknum > TP_MAX_BLOCK_NUMBER)
	{
		/* Skip this result and try the next one */
		so->current_pos++;
		if (so->current_pos >= so->result_count)
			return false;
		/* Recursive call to try the next result */
		return tp_gettuple(scan, dir);
	}

	scan->xs_heaptid		= so->result_ctids[so->current_pos];
	scan->xs_recheck		= false;
	scan->xs_recheckorderby = false;

	/* Set ORDER BY distance value */
	if (scan->numberOfOrderBys > 0)
	{
		float4 raw_score;

		Assert(scan->numberOfOrderBys == 1);
		Assert(scan->xs_orderbyvals != NULL);
		Assert(scan->xs_orderbynulls != NULL);
		Assert(so->result_scores != NULL);

		/* Convert BM25 score to Datum (ensure negative for ASC sort) */
		raw_score				 = so->result_scores[so->current_pos];
		bm25_score				 = (raw_score > 0) ? -raw_score : raw_score;
		scan->xs_orderbyvals[0]	 = Float4GetDatum(bm25_score);
		scan->xs_orderbynulls[0] = false;

		/* Log BM25 score if enabled */
		elog(tp_log_scores ? NOTICE : DEBUG1,
			 "BM25 index scan: tid=(%u,%u), BM25_score=%.4f",
			 BlockIdGetBlockNumber(&scan->xs_heaptid.ip_blkid),
			 scan->xs_heaptid.ip_posid,
			 bm25_score);

		/* Cache score for stub function to retrieve */
		tp_cached_score = (float8)bm25_score;
	}

	/* Move to next position */
	so->current_pos++;

	return true;
}
