/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * index.c - BM25 index access method implementation
 */
#include <postgres.h>

#include <access/amapi.h>
#include <access/genam.h>
#include <access/reloptions.h>
#include <access/relscan.h>
#include <access/sdir.h>
#include <access/sysattr.h>
#include <access/table.h>
#include <access/tableam.h>
#include <catalog/namespace.h>
#include <catalog/pg_am.h>
#include <catalog/pg_inherits.h>
#include <catalog/pg_opclass.h>
#include <commands/progress.h>
#include <commands/vacuum.h>
#include <executor/spi.h>
#include <math.h>
#include <miscadmin.h>
#include <nodes/execnodes.h>
#include <nodes/pg_list.h>
#include <nodes/value.h>
#include <parser/scansup.h>
#include <storage/bufmgr.h>
#include <tsearch/ts_type.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/float.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/regproc.h>
#include <utils/rel.h>
#include <utils/selfuncs.h>
#include <utils/snapmgr.h>
#include <utils/syscache.h>

#include "constants.h"
#include "dump.h"
#include "index.h"
#include "limit.h"
#include "memory.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "metapage.h"
#include "operator.h"
#include "query.h"
#include "segment/segment.h"
#include "segment/segment_merge.h"
#include "vector.h"

/* Relation options - initialized in mod.c */
extern relopt_kind tp_relopt_kind;

/* Local helper functions */
static char *tp_get_qualified_index_name(Relation indexRelation);
static void
tp_auto_spill_if_needed(TpLocalIndexState *index_state, Relation index_rel);
static bool tp_search_posting_lists(
		IndexScanDesc	   scan,
		TpLocalIndexState *index_state,
		TpVector		  *query_vector,
		TpIndexMetaPage	   metap);
static void tp_rescan_cleanup_results(TpScanOpaque so);
static void
tp_rescan_validate_query_index(Oid query_index_oid, Relation indexRelation);
static void tp_rescan_process_orderby(
		IndexScanDesc	scan,
		ScanKey			orderbys,
		int				norderbys,
		TpIndexMetaPage metap);

/*
 * Get the appropriate index name for the given index relation.
 * Returns a qualified name (schema.index) if the index is not visible
 * in the search path, otherwise returns just the index name.
 */
static char *
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
 * Auto-spill memtable to disk segment when posting count threshold exceeded.
 * This is called after each document insert to check if spill is needed.
 * The threshold is controlled by pg_textsearch.memtable_spill_threshold GUC.
 */
static void
tp_auto_spill_if_needed(TpLocalIndexState *index_state, Relation index_rel)
{
	BlockNumber		segment_root;
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	TpMemtable	   *memtable;
	int64			total_postings;

	if (!index_state || !index_rel || !index_state->shared)
		return;

	/* Check if posting count threshold is exceeded */
	if (tp_memtable_spill_threshold <= 0)
		return;

	memtable = get_memtable(index_state);
	if (!memtable)
		return;

	total_postings = memtable->total_postings;
	if (total_postings < tp_memtable_spill_threshold)
		return;

	elog(DEBUG1,
		 "Auto-spill triggered: %ld posting entries >= threshold %d",
		 (long)total_postings,
		 tp_memtable_spill_threshold);

	/* Write the segment */
	segment_root = tp_write_segment_v2(index_state, index_rel);

	/* Clear memtable and update metapage if spill succeeded */
	if (segment_root != InvalidBlockNumber)
	{
		tp_clear_memtable(index_state);

		/*
		 * Clear docid pages since data is now in segment. This prevents
		 * recovery from re-indexing documents already persisted in segments,
		 * which would cause duplicate entries and slow recovery.
		 */
		tp_clear_docid_pages(index_rel);

		/* Link new segment as L0 chain head */
		metabuf = ReadBuffer(index_rel, 0);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		if (metap->level_heads[0] != InvalidBlockNumber)
		{
			/* Point new segment to old chain head */
			Buffer			 seg_buf;
			Page			 seg_page;
			TpSegmentHeader *seg_header;

			seg_buf = ReadBuffer(index_rel, segment_root);
			LockBuffer(seg_buf, BUFFER_LOCK_EXCLUSIVE);
			seg_page				 = BufferGetPage(seg_buf);
			seg_header				 = (TpSegmentHeader *)((char *)seg_page +
											   SizeOfPageHeaderData);
			seg_header->next_segment = metap->level_heads[0];
			MarkBufferDirty(seg_buf);
			UnlockReleaseBuffer(seg_buf);
		}

		metap->level_heads[0] = segment_root;
		metap->level_counts[0]++;
		MarkBufferDirty(metabuf);
		UnlockReleaseBuffer(metabuf);

		elog(DEBUG1,
			 "Auto-spilled memtable to segment at block %u (L0 count: %u)",
			 segment_root,
			 metap->level_counts[0]);

		/* Check if L0 needs compaction */
		tp_maybe_compact_level(index_rel, 0);
	}
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
 * 32 levels is more than enough for any real partition hierarchy.
 */
#define MAX_INHERITANCE_DEPTH 32

/*
 * Check if child_oid inherits from ancestor_oid via pg_inherits.
 * Walks up the inheritance chain to handle multi-level partitions.
 *
 * Works for both indexes (partition indexes) and tables (inheritance).
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
 * Check if two BM25 indexes match by attribute (Sven's approach for
 * hypertables).
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
	 * Check if the scan index's table inherits from the query index's table
	 * and both are BM25 indexes on the same column.
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
			 * sk_subtype contains the right-hand operand's type OID:
			 * - TEXTOID for text <@> text
			 * - bm25query type OID for text <@> bm25query
			 *
			 * The planner hook transforms text <@> text to text <@> bm25query,
			 * so in practice we should always receive bm25query here.
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
 * Tapir Index Access Method Implementation
 *
 * This implements a custom PostgreSQL access method for ranked BM25 search.
 */

/* Index options */
typedef struct TpOptions
{
	int32  vl_len_;			   /* varlena header (do not touch directly!) */
	int32  text_config_offset; /* offset to text config string */
	double k1;				   /* BM25 k1 parameter */
	double b;				   /* BM25 b parameter */
} TpOptions;

/* Helper functions for tp_build */
static void
tp_build_extract_options(
		Relation index,
		char   **text_config_name,
		Oid		*text_config_oid,
		double	*k1,
		double	*b)
{
	TpOptions *options;

	*text_config_name = NULL;
	*text_config_oid  = InvalidOid;

	/* Extract options from index */
	options = (TpOptions *)index->rd_options;
	if (options)
	{
		if (options->text_config_offset > 0)
		{
			*text_config_name = pstrdup(
					(char *)options + options->text_config_offset);
			/* Convert text config name to OID */
			{
				List *names = list_make1(makeString(*text_config_name));

				*text_config_oid = get_ts_config_oid(names, false);
				list_free(names);
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text_config parameter is required for tapir "
							"indexes"),
					 errhint("Specify text_config when creating the index: "
							 "CREATE INDEX ... USING "
							 "tapir(column) WITH (text_config='english')")));
		}

		*k1 = options->k1;
		*b	= options->b;
	}
	else
	{
		/* No options provided - require text_config */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text_config parameter is required for tapir indexes"),
				 errhint("Specify text_config when creating the index: "
						 "CREATE INDEX ... USING "
						 "tapir(column) WITH (text_config='english')")));
	}
}

static void
tp_build_init_metapage(
		Relation index, Oid text_config_oid, double k1, double b)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;

	/* Initialize metapage */
	metabuf = ReadBuffer(index, P_NEW);
	Assert(BufferGetBlockNumber(metabuf) == TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);

	tp_init_metapage(metapage, text_config_oid);
	metap	  = (TpIndexMetaPage)PageGetContents(metapage);
	metap->k1 = k1;
	metap->b  = b;

	MarkBufferDirty(metabuf);

	/* Flush metapage to disk immediately to ensure crash recovery works */
	FlushOneBuffer(metabuf);

	UnlockReleaseBuffer(metabuf);
}

/*
 * Calculate the sum of all IDF values for the index
 */
void
tp_calculate_idf_sum(TpLocalIndexState *index_state)
{
	TpMemtable		  *memtable;
	dshash_table	  *string_table;
	dshash_seq_status  status;
	TpStringHashEntry *entry;
	float8			   idf_sum = 0.0;
	int32			   total_docs;
	int32			   term_count = 0;

	Assert(index_state != NULL);
	Assert(index_state->shared != NULL);

	total_docs = index_state->shared->total_docs;
	if (total_docs == 0)
		return; /* No documents, no IDF to calculate */

	memtable = get_memtable(index_state);
	if (!memtable || memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
		return;

	/* Attach to the string hash table */
	string_table = tp_string_table_attach(
			index_state->dsa, memtable->string_hash_handle);

	/* Iterate through all terms and calculate IDF for each */
	dshash_seq_init(&status, string_table, false); /* shared lock */

	while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) != NULL)
	{
		if (DsaPointerIsValid(entry->key.posting_list))
		{
			TpPostingList *posting_list =
					dsa_get_address(index_state->dsa, entry->key.posting_list);

			/* Calculate RAW IDF for this term (no epsilon adjustment) */
			double idf_numerator   = (double)(total_docs -
											  posting_list->doc_count + 0.5);
			double idf_denominator = (double)(posting_list->doc_count + 0.5);
			double idf_ratio	   = idf_numerator / idf_denominator;
			double raw_idf		   = log(idf_ratio);

			/* Use raw IDF for sum calculation (including negative values) */
			idf_sum += raw_idf;
			term_count++;
		}
	}

	dshash_seq_term(&status);
	dshash_detach(string_table);

	/* Store the IDF sum in shared state */
	index_state->shared->idf_sum = idf_sum;

	/* Update the term count in memtable */
	memtable->total_terms = term_count;
}

static void
tp_build_finalize_and_update_stats(
		Relation		   index,
		TpLocalIndexState *index_state,
		uint64			  *total_docs,
		uint64			  *total_len)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;

	Assert(index_state != NULL);

	/* Calculate IDF sum for average IDF computation */
	tp_calculate_idf_sum(index_state);

	/* Get actual statistics from the shared state */
	*total_docs = index_state->shared->total_docs;
	*total_len	= index_state->shared->total_len;

	/* Update metapage with computed statistics */
	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	metap->total_docs = *total_docs;
	metap->total_len  = *total_len;

	MarkBufferDirty(metabuf);

	/* Flush metapage to disk immediately to ensure crash recovery works */
	FlushOneBuffer(metabuf);

	UnlockReleaseBuffer(metabuf);
}

/*
 * Access method handler - returns IndexAmRoutine with function pointers
 */
PG_FUNCTION_INFO_V1(tp_handler);

Datum
tp_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine;

	(void)fcinfo; /* unused */

	amroutine = makeNode(IndexAmRoutine);

	amroutine->amstrategies	  = 0; /* No search strategies - ORDER BY only */
	amroutine->amsupport	  = 8; /* 8 for distance */
	amroutine->amoptsprocnum  = 0;
	amroutine->amcanorder	  = false;
	amroutine->amcanorderbyop = true; /* Supports ORDER BY operators */
#if PG_VERSION_NUM >= 180000
	amroutine->amcanhash			= false;
	amroutine->amconsistentequality = false;
	amroutine->amconsistentordering =
			true; /* Support consistent ordering for ORDER BY */
#endif
	amroutine->amcanbackward	  = false; /* Cannot scan backwards */
	amroutine->amcanunique		  = false; /* Cannot enforce uniqueness */
	amroutine->amcanmulticol	  = false; /* Single column only */
	amroutine->amoptionalkey	  = true;  /* Can scan without search key */
	amroutine->amsearcharray	  = false; /* No array search support */
	amroutine->amsearchnulls	  = false; /* Cannot search for NULLs */
	amroutine->amstorage		  = false; /* No separate storage type */
	amroutine->amclusterable	  = false; /* Cannot cluster on this index */
	amroutine->ampredlocks		  = false; /* No predicate locking */
	amroutine->amcanparallel	  = false; /* No parallel scan support yet */
	amroutine->amcanbuildparallel = true;
	amroutine->amcaninclude		  = false;		/* No INCLUDE columns */
	amroutine->amusemaintenanceworkmem = false; /* Use work_mem for builds */
	amroutine->amsummarizing		   = false;
	amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
	amroutine->amkeytype			   = InvalidOid;

	/* Interface functions */
	amroutine->ambuild			= tp_build;
	amroutine->ambuildempty		= tp_buildempty;
	amroutine->aminsert			= tp_insert;
	amroutine->aminsertcleanup	= NULL;
	amroutine->ambulkdelete		= tp_bulkdelete;
	amroutine->amvacuumcleanup	= tp_vacuumcleanup;
	amroutine->amcanreturn		= NULL;
	amroutine->amcostestimate	= tp_costestimate;
	amroutine->amoptions		= tp_options;
	amroutine->amproperty		= NULL;				 /* No property function */
	amroutine->ambuildphasename = tp_buildphasename; /* No build phase names */
	amroutine->amvalidate		= tp_validate;
	amroutine->amadjustmembers	= NULL; /* No member adjustment */
	amroutine->ambeginscan		= tp_beginscan;
	amroutine->amrescan			= tp_rescan;
	amroutine->amgettuple		= tp_gettuple;
	amroutine->amgetbitmap		= NULL; /* No bitmap scans - ORDER BY only like
										 * pgvector */
	amroutine->amendscan			  = tp_endscan;
	amroutine->ammarkpos			  = NULL; /* No mark/restore support */
	amroutine->amrestrpos			  = NULL;
	amroutine->amestimateparallelscan = NULL; /* No parallel support yet */
	amroutine->aminitparallelscan	  = NULL;
	amroutine->amparallelrescan		  = NULL;

#if PG_VERSION_NUM >= 180000
	amroutine->amtranslatestrategy = NULL;
	amroutine->amtranslatecmptype  = NULL;
#endif

	PG_RETURN_POINTER(amroutine);
}

IndexBulkDeleteResult *
tp_bulkdelete(
		IndexVacuumInfo		   *info,
		IndexBulkDeleteResult  *stats,
		IndexBulkDeleteCallback callback __attribute__((unused)),
		void				   *callback_state __attribute__((unused)))
{
	TpIndexMetaPage metap;

	/* Initialize stats structure if not provided */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	/* Get current index statistics from metapage */
	metap = tp_get_metapage(info->index);
	if (metap)
	{
		stats->num_pages		= 1; /* Minimal pages (just metapage) */
		stats->num_index_tuples = (double)metap->total_docs;

		/* Track that deletion was requested */
		stats->tuples_removed = 0;
		stats->pages_deleted  = 0;

		pfree(metap);
	}
	else
	{
		/* Couldn't read metapage, return minimal stats */
		stats->num_pages		= 0;
		stats->num_index_tuples = 0;
		stats->tuples_removed	= 0;
		stats->pages_deleted	= 0;

		elog(WARNING,
			 "Tapir bulkdelete: couldn't read metapage for index %s",
			 RelationGetRelationName(info->index));
	}

	return stats;
}

/* Tapir-specific build phases */
#define TP_PHASE_LOADING 2
#define TP_PHASE_WRITING 3

/* Progress reporting interval (tuples) */
#define TP_PROGRESS_REPORT_INTERVAL 1000

char *
tp_buildphasename(int64 phase)
{
	switch (phase)
	{
	case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
		return "initializing";
	case TP_PHASE_LOADING:
		return "loading tuples";
	case TP_PHASE_WRITING:
		return "writing index";
	default:
		return NULL;
	}
}

/*
 * Extract terms and frequencies from a TSVector
 *
 * Returns the document length (sum of all term frequencies)
 */
static int
tp_extract_terms_from_tsvector(
		TSVector tsvector,
		char  ***terms_out,
		int32  **frequencies_out,
		int		*term_count_out)
{
	int		   term_count = tsvector->size;
	char	 **terms;
	int32	  *frequencies;
	int		   doc_length = 0;
	int		   i;
	WordEntry *we;

	*term_count_out = term_count;

	if (term_count == 0)
	{
		*terms_out		 = NULL;
		*frequencies_out = NULL;
		return 0;
	}

	we = ARRPTR(tsvector);

	terms		= palloc(term_count * sizeof(char *));
	frequencies = palloc(term_count * sizeof(int32));

	for (i = 0; i < term_count; i++)
	{
		char *lexeme_start = STRPTR(tsvector) + we[i].pos;
		int	  lexeme_len   = we[i].len;
		char *lexeme;

		/* Always allocate on heap for terms array since we need to keep them
		 */
		lexeme = palloc(lexeme_len + 1);
		memcpy(lexeme, lexeme_start, lexeme_len);
		lexeme[lexeme_len] = '\0';

		terms[i] = lexeme;

		/*
		 * Get frequency from TSVector - count positions or default to 1
		 */
		if (we[i].haspos)
			frequencies[i] = (int32)POSDATALEN(tsvector, &we[i]);
		else
			frequencies[i] = 1;

		doc_length += frequencies[i];
	}

	*terms_out		 = terms;
	*frequencies_out = frequencies;

	return doc_length;
}

/*
 * Free memory allocated for terms array
 */
static void
tp_free_terms_array(char **terms, int term_count)
{
	int i;

	if (terms == NULL)
		return;

	for (i = 0; i < term_count; i++)
		pfree(terms[i]);

	pfree(terms);
}

/*
 * Setup table scanning for index build
 * Returns the snapshot (PG18+ only) for later unregistration
 */
static Snapshot
tp_setup_table_scan(
		Relation heap, TableScanDesc *scan_out, TupleTableSlot **slot_out)
{
	Snapshot snapshot = NULL;

#if PG_VERSION_NUM >= 180000
	/* PG18: Must register the snapshot for index builds */
	snapshot = GetTransactionSnapshot();
	if (snapshot)
		snapshot = RegisterSnapshot(snapshot);
	*scan_out = table_beginscan(heap, snapshot, 0, NULL);
#else
	*scan_out = table_beginscan(heap, GetTransactionSnapshot(), 0, NULL);
#endif

	*slot_out = table_slot_create(heap, NULL);

	return snapshot;
}

/*
 * Core document processing: convert text to terms and add to posting lists
 * This is shared between index building and docid recovery.
 *
 * If index_rel is provided, auto-spill will occur when memory limit is
 * exceeded. If index_rel is NULL, no auto-spill occurs (recovery path).
 */
bool
tp_process_document_text(
		text			  *document_text,
		ItemPointer		   ctid,
		Oid				   text_config_oid,
		TpLocalIndexState *index_state,
		Relation		   index_rel,
		int32			  *doc_length_out)
{
	char	*document_str;
	Datum	 tsvector_datum;
	TSVector tsvector;
	char   **terms;
	int32	*frequencies;
	int		 term_count;
	int		 doc_length;

	if (!document_text || !index_state)
		return false;

	document_str = text_to_cstring(document_text);

	/* Validate the TID before processing */
	if (!ItemPointerIsValid(ctid))
	{
		elog(WARNING,
			 "Invalid TID during document processing, skipping document");
		pfree(document_str);
		return false;
	}

	/* Vectorize the document using text configuration */
	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid, /* collation */
			ObjectIdGetDatum(text_config_oid),
			PointerGetDatum(document_text));

	tsvector = DatumGetTSVector(tsvector_datum);

	/* Extract lexemes and frequencies from TSVector */
	doc_length = tp_extract_terms_from_tsvector(
			tsvector, &terms, &frequencies, &term_count);

	if (term_count > 0)
	{
		/*
		 * Acquire exclusive lock for this transaction if not already held.
		 * During index build, we acquire once and hold for the entire build.
		 */
		tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

		/* Add document terms to posting lists */
		tp_add_document_terms(
				index_state, ctid, terms, frequencies, term_count, doc_length);

		/*
		 * Check memory after document completion and auto-spill if needed.
		 * Only spill if index_rel is provided (not during recovery).
		 */
		if (index_rel != NULL)
			tp_auto_spill_if_needed(index_state, index_rel);

		/* Free the terms array and individual lexemes */
		tp_free_terms_array(terms, term_count);
		pfree(frequencies);
	}

	if (doc_length_out)
		*doc_length_out = doc_length;

	pfree(document_str);
	return true;
}

/*
 * Process a single document during index build
 *
 * Returns true if document was processed successfully, false to skip
 */
static bool
tp_process_document(
		TupleTableSlot	  *slot,
		IndexInfo		  *indexInfo,
		Oid				   text_config_oid,
		TpLocalIndexState *index_state,
		Relation		   index,
		uint64			  *total_docs)
{
	bool		isnull;
	Datum		text_datum;
	text	   *document_text;
	ItemPointer ctid;
	int32		doc_length;

	/* Get the text column value (first indexed column) */
	text_datum =
			slot_getattr(slot, indexInfo->ii_IndexAttrNumbers[0], &isnull);

	if (isnull)
		return false; /* Skip NULL documents */

	document_text = DatumGetTextPP(text_datum);

	/* Ensure the slot is materialized to get the TID */
	slot_getallattrs(slot);
	ctid = &slot->tts_tid;

	/* Process the document text using shared helper */
	if (!tp_process_document_text(
				document_text,
				ctid,
				text_config_oid,
				index_state,
				index,
				&doc_length))
	{
		return false;
	}

	/* Store the docid for crash recovery (only during index build) */
	tp_add_docid_to_pages(index, ctid);

	(*total_docs)++;
	return true;
}

/*
 * Build a new Tapir index
 */
IndexBuildResult *
tp_build(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult  *result;
	char			  *text_config_name = NULL;
	Oid				   text_config_oid	= InvalidOid;
	double			   k1, b;
	TableScanDesc	   scan;
	TupleTableSlot	  *slot;
	Snapshot		   snapshot	  = NULL;
	uint64			   total_docs = 0;
	uint64			   total_len  = 0;
	TpLocalIndexState *index_state;

	/* BM25 index build started */
	elog(NOTICE,
		 "BM25 index build started for relation %s",
		 RelationGetRelationName(index));

	/*
	 * Invalidate docid cache to prevent stale entries from a previous build.
	 * This is critical during VACUUM FULL, which creates a new index file
	 * with different block layout than the old one.
	 */
	tp_invalidate_docid_cache();

	/*
	 * Check for expression indexes - BM25 indexes must be on a direct column
	 * reference, not an expression like lower(content).
	 *
	 * For expression indexes, ii_IndexAttrNumbers[0] is 0 because the index
	 * is on an expression rather than a table column. Our tp_process_document
	 * uses slot_getattr() which requires a valid attribute number >= 1.
	 */
	if (indexInfo->ii_IndexAttrNumbers[0] == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BM25 indexes on expressions are not supported"),
				 errhint("Create the index on a column directly, e.g., "
						 "CREATE INDEX ... USING bm25(content)")));
	}

	/* Report initialization phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE,
			PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE);

	/* Extract options from index */
	tp_build_extract_options(
			index, &text_config_name, &text_config_oid, &k1, &b);

	/* Log configuration being used */
	if (text_config_name)
		elog(NOTICE, "Using text search configuration: %s", text_config_name);
	elog(NOTICE, "Using index options: k1=%.2f, b=%.2f", k1, b);

	/* Initialize metapage */
	tp_build_init_metapage(index, text_config_oid, k1, b);

	/* Initialize index state */
	index_state = tp_create_shared_index_state(
			RelationGetRelid(index), RelationGetRelid(heap));

	/* Report loading phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);

	/*
	 * Report estimated tuple count for progress tracking.
	 * Use reltuples for estimate (may be -1 if never analyzed).
	 */
	{
		double reltuples  = heap->rd_rel->reltuples;
		int64  tuples_est = (reltuples > 0) ? (int64)reltuples : 0;

		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_TOTAL, tuples_est);
	}

	/* Prepare to scan table */
	snapshot = tp_setup_table_scan(heap, &scan, &slot);

	/* Process each document in the heap */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		tp_process_document(
				slot,
				indexInfo,
				text_config_oid,
				index_state,
				index,
				&total_docs);

		/* Report progress every TP_PROGRESS_REPORT_INTERVAL tuples */
		if (total_docs % TP_PROGRESS_REPORT_INTERVAL == 0)
		{
			pgstat_progress_update_param(
					PROGRESS_CREATEIDX_TUPLES_DONE, total_docs);

			/* Allow query cancellation */
			CHECK_FOR_INTERRUPTS();
		}
	}

	/* Report final tuple count */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, total_docs);

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

#if PG_VERSION_NUM >= 180000
	/* Unregister the snapshot (PG18+ only) */
	if (snapshot)
		UnregisterSnapshot(snapshot);
#endif

	/* Report writing phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_WRITING);

	/* Finalize posting lists and update statistics */
	tp_build_finalize_and_update_stats(
			index, index_state, &total_docs, &total_len);

	/* Create index build result */
	result				= (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
	result->heap_tuples = total_docs;
	result->index_tuples = total_docs;

	/* Calculate and log average IDF if we have terms */
	/* IDF sum has been calculated and stored in shared state */

	if (OidIsValid(text_config_oid))
	{
		elog(NOTICE,
			 "BM25 index build completed: " UINT64_FORMAT
			 " documents, avg_length=%.2f, "
			 "text_config='%s' (k1=%.2f, b=%.2f)",
			 total_docs,
			 total_len > 0 ? (float4)(total_len / (double)total_docs) : 0.0,
			 text_config_name ? text_config_name : "unknown",
			 k1,
			 b);
	}
	else
	{
		elog(NOTICE,
			 "BM25 index build completed: " UINT64_FORMAT
			 " documents, avg_length=%.2f "
			 "(text_config=%s, k1=%.2f, b=%.2f)",
			 total_docs,
			 total_len > 0 ? (float4)(total_len / (double)total_docs) : 0.0,
			 text_config_name,
			 k1,
			 b);
	}

	/* Report FSM page reuse statistics */
	tp_report_fsm_stats();

	return result;
}

/*
 * Build an empty Tapir index (for CREATE INDEX without data)
 */
void
tp_buildempty(Relation index)
{
	TpOptions	   *options;
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	char		   *text_config_name = NULL;
	Oid				text_config_oid	 = InvalidOid;

	/* Extract options from index */
	options = (TpOptions *)index->rd_options;
	if (options)
	{
		if (options->text_config_offset > 0)
		{
			text_config_name = pstrdup(
					(char *)options + options->text_config_offset);
			{
				List *names = list_make1(makeString(text_config_name));

				text_config_oid = get_ts_config_oid(names, false);
				list_free(names);
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text_config parameter is required for tapir "
							"indexes"),
					 errhint("Specify text_config when creating the index: "
							 "CREATE INDEX ... USING "
							 "tapir(column) WITH (text_config='english')")));
		}
	}
	else
	{
		/* No options provided - require text_config */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text_config parameter is required for tapir indexes"),
				 errhint("Specify text_config when creating the index: "
						 "CREATE INDEX ... USING "
						 "tapir(column) WITH (text_config='english')")));
	}

	/* Create and initialize the metapage */
	metabuf = ReadBuffer(index, P_NEW);
	Assert(BufferGetBlockNumber(metabuf) == TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	metapage = BufferGetPage(metabuf);
	tp_init_metapage(metapage, text_config_oid);

	/* Set additional parameters after init */
	metap			   = (TpIndexMetaPage)PageGetContents(metapage);
	metap->k1		   = TP_DEFAULT_K1;
	metap->b		   = TP_DEFAULT_B;
	metap->total_docs  = 0;
	metap->total_terms = 0;
	metap->total_len   = 0;

	MarkBufferDirty(metabuf);

	/* Flush metapage to disk immediately to ensure crash recovery works */
	FlushOneBuffer(metabuf);

	UnlockReleaseBuffer(metabuf);
}

/*
 * Insert a tuple into the Tapir index
 */
bool
tp_insert(
		Relation		 index,
		Datum			*values,
		bool			*isnull,
		ItemPointer		 ht_ctid,
		Relation		 heapRel,
		IndexUniqueCheck checkUnique,
		bool			 indexUnchanged,
		IndexInfo		*indexInfo)
{
	text			  *document_text;
	Datum			   vector_datum;
	TpVector		  *tpvec;
	TpVectorEntry	  *vector_entry;
	int32			  *frequencies;
	int				   term_count;
	int				   doc_length = 0;
	int				   i;
	TpLocalIndexState *index_state;

	(void)heapRel;		  /* unused */
	(void)checkUnique;	  /* unused */
	(void)indexUnchanged; /* unused */
	(void)indexInfo;	  /* unused */

	/* Skip NULL documents */
	if (isnull[0])
		return true;

	/* Get index state */
	index_state = tp_get_local_index_state(RelationGetRelid(index));

	/*
	 * Acquire exclusive lock for this transaction if not already held.
	 * This ensures memory consistency on NUMA systems and serializes
	 * write transactions with respect to reads.
	 */
	if (index_state != NULL)
	{
		tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
	}

	/* Extract text from first column */
	document_text = DatumGetTextPP(values[0]);

	/* Vectorize the document */
	{
		char *index_name;
		char *schema_name;
		Oid	  namespace_oid = RelationGetNamespace(index);

		schema_name = get_namespace_name(namespace_oid);
		index_name =
				psprintf("%s.%s", schema_name, RelationGetRelationName(index));

		vector_datum = DirectFunctionCall2(
				to_tpvector,
				PointerGetDatum(document_text),
				CStringGetTextDatum(index_name));

		pfree(index_name);
	}
	tpvec = (TpVector *)DatumGetPointer(vector_datum);

	/* Extract term IDs and frequencies from tpvector */
	term_count = tpvec->entry_count;
	if (term_count > 0)
	{
		char **terms = palloc(term_count * sizeof(char *));

		frequencies = palloc(term_count * sizeof(int32));

		vector_entry = TPVECTOR_ENTRIES_PTR(tpvec);
		for (i = 0; i < term_count; i++)
		{
			char *lexeme;

			/*
			 * Always allocate on heap for terms array since we need to keep
			 * them
			 */
			lexeme = palloc(vector_entry->lexeme_len + 1);
			memcpy(lexeme, vector_entry->lexeme, vector_entry->lexeme_len);
			lexeme[vector_entry->lexeme_len] = '\0';

			/* Store the lexeme string directly in terms array */
			terms[i]	   = lexeme;
			frequencies[i] = vector_entry->frequency;
			doc_length += vector_entry->frequency;

			vector_entry = get_tpvector_next_entry(vector_entry);
		}

		/* Add document terms to posting lists (if shared memory available) */
		if (index_state != NULL)
		{
			/* Validate TID before adding to posting list */
			if (!ItemPointerIsValid(ht_ctid))
				elog(WARNING, "Invalid TID in tp_insert, skipping");
			else
			{
				tp_add_document_terms(
						index_state,
						ht_ctid,
						terms,
						frequencies,
						term_count,
						doc_length);

				/* Auto-spill if memory limit exceeded */
				tp_auto_spill_if_needed(index_state, index);
			}
		}

		/* Free the terms array and individual lexemes */
		for (i = 0; i < term_count; i++)
			pfree(terms[i]);
		pfree(terms);
		pfree(frequencies);
	}

	/* Store the docid for crash recovery */
	tp_add_docid_to_pages(index, ht_ctid);

	/* Recalculate IDF sum after insert */
	tp_calculate_idf_sum(index_state);

	return true;
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
	 * Custom index AMs must allocate ORDER BY arrays themselves. This follows
	 * the pattern from GiST and SP-GiST.
	 */
	if (norderbys > 0)
	{
		scan->xs_orderbyvals  = (Datum *)palloc0(sizeof(Datum) * norderbys);
		scan->xs_orderbynulls = (bool *)palloc(sizeof(bool) * norderbys);
		/* Initialize all orderbynulls to true, as GiST and SP-GiST do */
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
	 * Don't free ORDER BY arrays here - they're allocated in our beginscan
	 * but PostgreSQL's core code expects them to persist and will free them.
	 * Just NULL out the pointers for safety.
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
	PG_TRY();
	{
		metap = tp_get_metapage(scan->indexRelation);
		if (!metap)
		{
			elog(WARNING,
				 "Failed to get metapage for index %s",
				 RelationGetRelationName(scan->indexRelation));
			return false;
		}
	}
	PG_CATCH();
	{
		elog(WARNING,
			 "Exception while getting metapage for index %s",
			 RelationGetRelationName(scan->indexRelation));
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Perform actual BM25 search using posting lists */
	PG_TRY();
	{
		/* Get the index state with posting lists */
		index_state = tp_get_local_index_state(
				RelationGetRelid(scan->indexRelation));

		if (!index_state)
		{
			elog(WARNING, "Could not get index state for BM25 search");
			pfree(metap);
			return false;
		}

		/* Acquire shared lock for reading the memtable */
		tp_acquire_index_lock(index_state, LW_SHARED);

		/* Use the original query vector stored during rescan, or create one
		 * from text */
		query_vector = so->query_vector;

		if (!query_vector && so->query_text)
		{
			/*
			 * We have a text query - convert it to a vector using the index.
			 * We need to use a qualified name if the index is not in the
			 * search path.
			 */
			char *index_name = tp_get_qualified_index_name(
					scan->indexRelation);

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
			elog(WARNING, "No query vector available in scan state");
			pfree(metap);
			return false;
		}

		/* Find documents matching the query using posting lists */
		success = tp_search_posting_lists(
				scan, index_state, query_vector, metap);
	}
	PG_CATCH();
	{
		success = false;
	}
	PG_END_TRY();

	pfree(metap);
	return success;
}

/*
 * Search posting lists for documents matching the query vector
 * Returns true on success, false on failure
 */
static bool
tp_search_posting_lists(
		IndexScanDesc	   scan,
		TpLocalIndexState *index_state,
		TpVector		  *query_vector,
		TpIndexMetaPage	   metap)
{
	TpScanOpaque so = (TpScanOpaque)scan->opaque;
	int			 max_results;
	int			 result_count = 0;
	float4		 k1_value;
	float4		 b_value;

	/* Extract terms and frequencies from query vector */
	char		 **query_terms;
	int32		  *query_frequencies;
	TpVectorEntry *entries_ptr;
	int			   entry_count;
	char		  *ptr;
	MemoryContext  oldcontext;

	/* Use limit from scan state, fallback to GUC parameter */
	if (so && so->limit > 0)
		max_results = so->limit;
	else
		max_results = tp_default_limit;

	entry_count		  = query_vector->entry_count;
	query_terms		  = palloc(entry_count * sizeof(char *));
	query_frequencies = palloc(entry_count * sizeof(int32));
	entries_ptr		  = TPVECTOR_ENTRIES_PTR(query_vector);

	/* Parse the query vector entries */
	ptr = (char *)entries_ptr;
	for (int i = 0; i < entry_count; i++)
	{
		TpVectorEntry *entry = (TpVectorEntry *)ptr;
		char		  *term_str;

		/*
		 * Always allocate on heap for query terms array since we need to keep
		 * them
		 */
		term_str = palloc(entry->lexeme_len + 1);
		memcpy(term_str, entry->lexeme, entry->lexeme_len);
		term_str[entry->lexeme_len] = '\0';

		/* Store the term string directly in query terms array */
		query_terms[i]		 = term_str;
		query_frequencies[i] = entry->frequency;

		ptr += sizeof(TpVectorEntry) + MAXALIGN(entry->lexeme_len);
	}

	/* Allocate result arrays in scan context */
	oldcontext		 = MemoryContextSwitchTo(so->scan_context);
	so->result_ctids = palloc(max_results * sizeof(ItemPointerData));
	/* Initialize to invalid TIDs for safety */
	memset(so->result_ctids, 0, max_results * sizeof(ItemPointerData));
	MemoryContextSwitchTo(oldcontext);

	/* Extract values from metap */
	Assert(metap != NULL);
	k1_value = metap->k1;
	b_value	 = metap->b;

	Assert(index_state != NULL);
	Assert(query_terms != NULL);
	Assert(query_frequencies != NULL);
	Assert(so->result_ctids != NULL);

	result_count = tp_score_documents(
			index_state,
			scan->indexRelation,
			query_terms,
			query_frequencies,
			entry_count,
			k1_value,
			b_value,
			max_results,
			so->result_ctids,
			&so->result_scores);

	so->result_count = result_count;
	so->current_pos	 = 0;

	/* Free the query terms array and individual term strings */
	for (int i = 0; i < entry_count; i++)
		pfree(query_terms[i]);

	pfree(query_terms);
	pfree(query_frequencies);

	return result_count > 0;
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
	}

	/* Move to next position */
	so->current_pos++;

	return true;
}

/*
 * Estimate cost of BM25 index scan
 */
void
tp_costestimate(
		PlannerInfo *root,
		IndexPath	*path,
		double		 loop_count,
		Cost		*indexStartupCost,
		Cost		*indexTotalCost,
		Selectivity *indexSelectivity,
		double		*indexCorrelation,
		double		*indexPages)
{
	GenericCosts	costs;
	TpIndexMetaPage metap;
	double			num_tuples = TP_DEFAULT_TUPLE_ESTIMATE;

	/* Never use index without ORDER BY clause */
	if (!path->indexorderbys || list_length(path->indexorderbys) == 0)
	{
		*indexStartupCost = get_float8_infinity();
		*indexTotalCost	  = get_float8_infinity();
		return;
	}

	/* Check for LIMIT clause and verify it can be safely pushed down */
	if (root && root->limit_tuples > 0 && root->limit_tuples < INT_MAX)
	{
		int limit = (int)root->limit_tuples;

		if (tp_can_pushdown_limit(root, path, limit))
			tp_store_query_limit(path->indexinfo->indexoid, limit);
	}

	/* Try to get actual statistics from the index */
	if (path->indexinfo && path->indexinfo->indexoid != InvalidOid)
	{
		Relation index_rel =
				index_open(path->indexinfo->indexoid, AccessShareLock);

		if (index_rel)
		{
			metap = tp_get_metapage(index_rel);
			if (metap && metap->total_docs > 0)
				num_tuples = (double)metap->total_docs;

			if (metap)
				pfree(metap);

			index_close(index_rel, AccessShareLock);
		}
	}

	/* Initialize generic costs */
	MemSet (&costs, 0, sizeof(costs))
		;
	genericcostestimate(root, path, loop_count, &costs);

	/* Override with BM25-specific estimates */
	*indexStartupCost = costs.indexStartupCost + 0.01; /* Small startup cost */
	*indexTotalCost	  = costs.indexTotalCost * TP_INDEX_SCAN_COST_FACTOR;

	/*
	 * Calculate selectivity based on LIMIT if available, otherwise use default
	 */
	if (root && root->limit_tuples > 0 && root->limit_tuples < INT_MAX &&
		num_tuples > 0)
	{
		/* Use LIMIT as upper bound for selectivity calculation */
		double limit_selectivity = Min(1.0, root->limit_tuples / num_tuples);
		*indexSelectivity =
				Max(limit_selectivity, TP_DEFAULT_INDEX_SELECTIVITY);
	}
	else
	{
		*indexSelectivity = TP_DEFAULT_INDEX_SELECTIVITY;
	}
	*indexCorrelation = 0.0; /* No correlation assumptions */
	*indexPages		  = Max(1.0, num_tuples / 100.0); /* Rough page estimate */
}

/*
 * Parse and validate index options
 */
bytea *
tp_options(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] =
			{{"text_config",
			  RELOPT_TYPE_STRING,
			  offsetof(TpOptions, text_config_offset),
			  0},
			 {"k1", RELOPT_TYPE_REAL, offsetof(TpOptions, k1), 0},
			 {"b", RELOPT_TYPE_REAL, offsetof(TpOptions, b), 0}};

	return (bytea *)build_reloptions(
			reloptions,
			validate,
			tp_relopt_kind,
			sizeof(TpOptions),
			tab,
			lengthof(tab));
}

/*
 * Validate BM25 index definition
 */
bool
tp_validate(Oid opclassoid)
{
	HeapTuple		tup;
	Form_pg_opclass opclassform;
	Oid				opcintype;
	bool			result = true;

	/* Look up the opclass */
	tup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(tup))
	{
		elog(WARNING, "cache lookup failed for operator class %u", opclassoid);
		return false;
	}

	opclassform = (Form_pg_opclass)GETSTRUCT(tup);
	opcintype	= opclassform->opcintype;

	switch (opcintype)
	{
	case TEXTOID:
	case VARCHAROID:
	case BPCHAROID: /* char(n) */
		result = true;
		break;
	default:
		elog(WARNING,
			 "Tapir index can only be created on text, varchar, or char "
			 "columns (got type OID %u)",
			 opcintype);
		result = false;
		break;
	}

	ReleaseSysCache(tup);

	return result;
}

/*
 * Vacuum/cleanup the BM25 index
 */
IndexBulkDeleteResult *
tp_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	TpIndexMetaPage metap;

	/* Initialize stats if not provided */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	/* Get current index statistics from metapage */
	metap = tp_get_metapage(info->index);
	if (metap)
	{
		/* Update statistics with current values */
		stats->num_pages		= 1; /* Minimal pages (just metapage) */
		stats->num_index_tuples = (double)metap->total_docs;

		/* Report current usage statistics */
		if (stats->pages_deleted == 0 && stats->tuples_removed == 0)
		{
			/* No deletions recorded, report full statistics */
			stats->pages_free = 0; /* No free pages in memtable
									* implementation */
		}

		pfree(metap);
	}
	else
	{
		elog(WARNING,
			 "Tapir vacuum cleanup: couldn't read metapage for index %s",
			 RelationGetRelationName(info->index));

		/* Keep existing stats if available, otherwise initialize */
		if (stats->num_pages == 0 && stats->num_index_tuples == 0)
		{
			stats->num_pages		= 1; /* At least the metapage */
			stats->num_index_tuples = 0;
		}
	}

	return stats;
}

/*
 * tp_dump_index - Debug function to show internal index structure
 * including both memtable and all segments
 *
 * Takes index name and optional filename. If filename is provided,
 * writes full dump to file (no truncation, includes hex dumps).
 * Otherwise returns truncated output as text.
 */
PG_FUNCTION_INFO_V1(tp_dump_index);

Datum
tp_dump_index(PG_FUNCTION_ARGS)
{
	text *index_name_text = PG_GETARG_TEXT_PP(0);
	char *index_name	  = text_to_cstring(index_name_text);

	/* Check for optional filename parameter */
	if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
	{
		/* File mode - full dump with hex */
		text *filename_text = PG_GETARG_TEXT_PP(1);
		char *filename		= text_to_cstring(filename_text);
		FILE *fp;

		fp = fopen(filename, "w");
		if (!fp)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", filename)));
		}

		{
			DumpOutput out;
			dump_init_file(&out, fp);
			tp_dump_index_to_output(index_name, &out);
		}

		fclose(fp);

		elog(INFO, "Index dump written to %s", filename);
		PG_RETURN_TEXT_P(cstring_to_text_with_len(filename, strlen(filename)));
	}
	else
	{
		/* String mode - truncated output for SQL return */
		StringInfoData result;
		DumpOutput	   out;

		initStringInfo(&result);
		dump_init_string(&out, &result);
		tp_dump_index_to_output(index_name, &out);

		PG_RETURN_TEXT_P(cstring_to_text(result.data));
	}
}

/*
 * tp_summarize_index - Show index statistics without dumping content
 *
 * A faster alternative to bm25_dump_index that shows only statistics:
 * corpus statistics, BM25 parameters, memory usage, memtable/segment counts.
 */
PG_FUNCTION_INFO_V1(tp_summarize_index);

Datum
tp_summarize_index(PG_FUNCTION_ARGS)
{
	text		  *index_name_text = PG_GETARG_TEXT_PP(0);
	char		  *index_name	   = text_to_cstring(index_name_text);
	StringInfoData result;
	DumpOutput	   out;

	initStringInfo(&result);
	dump_init_string(&out, &result);
	tp_summarize_index_to_output(index_name, &out);

	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/*
 * tp_spill_memtable - Force memtable flush to disk segment
 *
 * This function allows manual triggering of segment writes.
 * Returns the block number of the written segment, or NULL if memtable was
 * empty.
 */
PG_FUNCTION_INFO_V1(tp_spill_memtable);

Datum
tp_spill_memtable(PG_FUNCTION_ARGS)
{
	text			  *index_name_text = PG_GETARG_TEXT_PP(0);
	char			  *index_name	   = text_to_cstring(index_name_text);
	Oid				   index_oid;
	Relation		   index_rel;
	TpLocalIndexState *index_state;
	BlockNumber		   segment_root;
	RangeVar		  *rv;

	/* Parse index name (supports schema.index notation) */
	rv = makeRangeVarFromNameList(stringToQualifiedNameList(index_name, NULL));
	index_oid = RangeVarGetRelid(rv, AccessShareLock, false);

	if (!OidIsValid(index_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" does not exist", index_name)));

	/* Open the index */
	index_rel = index_open(index_oid, RowExclusiveLock);

	/* Get index state */
	index_state = tp_get_local_index_state(RelationGetRelid(index_rel));
	if (!index_state)
	{
		index_close(index_rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for \"%s\"", index_name)));
	}

	/* Acquire exclusive lock for write operation */
	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

	/* Write the segment */
	segment_root = tp_write_segment_v2(index_state, index_rel);

	/* Clear the memtable after successful spilling */
	if (segment_root != InvalidBlockNumber)
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;

		tp_clear_memtable(index_state);

		/* Link new segment as L0 chain head */
		metabuf = ReadBuffer(index_rel, 0);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		if (metap->level_heads[0] != InvalidBlockNumber)
		{
			/* Point new segment to old chain head */
			Buffer			 seg_buf;
			Page			 seg_page;
			TpSegmentHeader *seg_header;

			seg_buf = ReadBuffer(index_rel, segment_root);
			LockBuffer(seg_buf, BUFFER_LOCK_EXCLUSIVE);
			seg_page				 = BufferGetPage(seg_buf);
			seg_header				 = (TpSegmentHeader *)((char *)seg_page +
											   SizeOfPageHeaderData);
			seg_header->next_segment = metap->level_heads[0];
			MarkBufferDirty(seg_buf);
			UnlockReleaseBuffer(seg_buf);
		}

		metap->level_heads[0] = segment_root;
		metap->level_counts[0]++;
		MarkBufferDirty(metabuf);

		UnlockReleaseBuffer(metabuf);

		/* Check if L0 needs compaction */
		tp_maybe_compact_level(index_rel, 0);
	}

	/* Release lock */
	tp_release_index_lock(index_state);

	/* Close the index */
	index_close(index_rel, RowExclusiveLock);

	/* Return block number or NULL */
	if (segment_root != InvalidBlockNumber)
	{
		PG_RETURN_INT32(segment_root);
	}
	else
	{
		PG_RETURN_NULL();
	}
}
