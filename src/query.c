#include <postgres.h>

#include <access/genam.h>
#include <access/htup_details.h>
#include <access/relation.h>
#include <access/table.h>
#include <access/xact.h>
#include <catalog/indexing.h>
#include <catalog/namespace.h>
#include <catalog/pg_am.h>
#include <catalog/pg_class.h>
#include <catalog/pg_index.h>
#include <catalog/pg_inherits.h>
#include <commands/defrem.h>
#include <fmgr.h>
#include <lib/stringinfo.h>
#include <libpq/pqformat.h>
#include <nodes/pg_list.h>
#include <nodes/value.h>
#include <tsearch/ts_type.h>
#include <utils/builtins.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/regproc.h>
#include <utils/rel.h>
#include <utils/syscache.h>
#include <varatt.h>

#include "constants.h"
#include "index.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "metapage.h"
#include "operator.h"
#include "planner.h"
#include "query.h"
#include "segment/segment.h"
#include "state.h"
#include "vector.h"

/*
 * Cache for per-query IDF values to avoid repeated segment lookups.
 *
 * When the <@> operator is called per-row (e.g., ORDER BY text <@> query),
 * we need to calculate IDF for each query term. Without caching, this
 * requires opening all segments for EVERY row, which is catastrophically
 * slow with 1.86M rows and multiple segments.
 *
 * The cache is stored in fn_extra and persists for the duration of the
 * query. IDF values are computed once on first call and reused.
 */
#define MAX_CACHED_TERMS 64

typedef struct TermIdfEntry
{
	char   term[NAMEDATALEN]; /* null-terminated term string */
	uint32 doc_freq;		  /* unified doc frequency (memtable + segments) */
	float4 idf;				  /* cached IDF value */
} TermIdfEntry;

typedef struct QueryScoreCache
{
	Oid			 index_oid;		/* index this cache is for */
	BlockNumber	 first_segment; /* segment chain head at cache time */
	int32		 total_docs;	/* total docs at cache time */
	float4		 avg_doc_len;	/* avg doc length at cache time */
	int			 num_terms;		/* number of cached terms */
	TermIdfEntry terms[MAX_CACHED_TERMS];
} QueryScoreCache;

/* Local helper functions */

/*
 * Look up cached IDF for a term. Returns -1.0 if not found in cache.
 */
static float4
lookup_cached_idf(QueryScoreCache *cache, const char *term, uint32 *doc_freq)
{
	int i;

	if (!cache)
		return -1.0f;

	for (i = 0; i < cache->num_terms; i++)
	{
		if (strcmp(cache->terms[i].term, term) == 0)
		{
			if (doc_freq)
				*doc_freq = cache->terms[i].doc_freq;
			return cache->terms[i].idf;
		}
	}
	return -1.0f;
}

/*
 * Add a term's IDF to the cache.
 * Warns once per query if the cache limit is exceeded.
 */
static void
cache_term_idf(
		QueryScoreCache *cache, const char *term, uint32 doc_freq, float4 idf)
{
	if (!cache)
		return;

	if (cache->num_terms >= MAX_CACHED_TERMS)
	{
		/* Warn once when limit is first exceeded */
		if (cache->num_terms == MAX_CACHED_TERMS)
		{
			ereport(WARNING,
					(errmsg("BM25 IDF cache limit exceeded (%d terms), "
							"additional terms will not be cached",
							MAX_CACHED_TERMS)));
			cache->num_terms++; /* Prevent repeated warnings */
		}
		return;
	}

	strlcpy(cache->terms[cache->num_terms].term, term, NAMEDATALEN);
	cache->terms[cache->num_terms].doc_freq = doc_freq;
	cache->terms[cache->num_terms].idf		= idf;
	cache->num_terms++;
}

/*
 * Check if the cache is valid for the current index state.
 */
static bool
cache_is_valid(
		QueryScoreCache *cache,
		Oid				 index_oid,
		BlockNumber		 first_segment,
		int32			 total_docs)
{
	if (!cache)
		return false;
	if (cache->index_oid != index_oid)
		return false;
	if (cache->first_segment != first_segment)
		return false;
	if (cache->total_docs != total_docs)
		return false;
	return true;
}

PG_FUNCTION_INFO_V1(tpquery_in);
PG_FUNCTION_INFO_V1(tpquery_out);
PG_FUNCTION_INFO_V1(tpquery_recv);
PG_FUNCTION_INFO_V1(tpquery_send);
PG_FUNCTION_INFO_V1(to_tpquery_text);
PG_FUNCTION_INFO_V1(to_tpquery_text_index);
PG_FUNCTION_INFO_V1(text_tpquery_score);
PG_FUNCTION_INFO_V1(text_text_score);
PG_FUNCTION_INFO_V1(tp_distance);
PG_FUNCTION_INFO_V1(tpquery_eq);

/*
 * tpquery input function
 * Formats:
 *   "query_text" - simple query without index (InvalidOid)
 *   "index_name:query_text" - query with index name (resolved to OID)
 * Note: If query_text contains a colon, use to_tpquery() instead
 */
Datum
tpquery_in(PG_FUNCTION_ARGS)
{
	char	*str = PG_GETARG_CSTRING(0);
	char	*colon;
	TpQuery *result;

	/* Check for index name prefix (format: "index_name:query") */
	colon = strchr(str, ':');
	if (colon && colon != str)
	{
		/* Found colon and it's not at the start - extract index name */
		int	  index_name_len = colon - str;
		char *index_name	 = palloc(index_name_len + 1);
		char *query_text	 = colon + 1; /* Skip past : */

		/* Copy the index name */
		memcpy(index_name, str, index_name_len);
		index_name[index_name_len] = '\0';

		/* Create query with index name (resolves to OID) */
		result = create_tpquery_from_name(query_text, index_name);
		pfree(index_name);
	}
	else
	{
		/* No index name prefix - create without index */
		result = create_tpquery(str, InvalidOid);
	}

	PG_RETURN_POINTER(result);
}

/*
 * tpquery output function
 * Converts OID back to index name for display
 */
Datum
tpquery_out(PG_FUNCTION_ARGS)
{
	TpQuery	  *tpquery = (TpQuery *)PG_GETARG_POINTER(0);
	StringInfo str	   = makeStringInfo();

	if (OidIsValid(tpquery->index_oid))
	{
		/* Format with index name: "index_name:query_text" */
		char *index_name = get_rel_name(tpquery->index_oid);
		char *query_text = get_tpquery_text(tpquery);

		if (index_name)
			appendStringInfo(str, "%s:%s", index_name, query_text);
		else
			/* Index was dropped - show OID for debugging */
			appendStringInfo(
					str, "[oid=%u]:%s", tpquery->index_oid, query_text);
	}
	else
	{
		/* Format without index: just the query text */
		char *query_text = get_tpquery_text(tpquery);

		appendStringInfo(str, "%s", query_text);
	}

	PG_RETURN_CSTRING(str->data);
}

/*
 * tpquery receive function (binary input)
 * Binary format: version (1 byte) + index_oid (4 bytes) + query_text_len (4
 * bytes) + query_text
 */
Datum
tpquery_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
	TpQuery	  *result;
	uint8	   version;
	Oid		   index_oid;
	int32	   query_text_len;
	char	  *query_text;

	/* Read and validate version */
	version = pq_getmsgint(buf, sizeof(uint8));
	if (version != TPQUERY_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("unsupported bm25query binary format version %u",
						version),
				 errhint("Expected version %u. This may indicate data from "
						 "an incompatible pg_textsearch version.",
						 TPQUERY_VERSION)));

	index_oid	   = pq_getmsgint(buf, sizeof(Oid));
	query_text_len = pq_getmsgint(buf, sizeof(int32));

	/* Validate length to prevent unbounded memory allocation */
	if (query_text_len < 0 || query_text_len > 1000000) /* 1MB limit */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid query text length: %d", query_text_len)));

	query_text = palloc(query_text_len + 1);
	pq_copymsgbytes(buf, query_text, query_text_len);
	query_text[query_text_len] = '\0';

	result = create_tpquery(query_text, index_oid);
	pfree(query_text);

	PG_RETURN_POINTER(result);
}

/*
 * tpquery send function (binary output)
 * Binary format: version (1 byte) + index_oid (4 bytes) + query_text_len (4
 * bytes) + query_text
 */
Datum
tpquery_send(PG_FUNCTION_ARGS)
{
	TpQuery	  *tpquery = (TpQuery *)PG_GETARG_POINTER(0);
	StringInfo buf	   = makeStringInfo();
	char	  *query_text;

	pq_sendint8(buf, TPQUERY_VERSION);
	pq_sendint32(buf, tpquery->index_oid);
	pq_sendint32(buf, tpquery->query_text_len);

	query_text = get_tpquery_text(tpquery);
	pq_sendbytes(buf, query_text, tpquery->query_text_len);

	PG_RETURN_BYTEA_P(pq_endtypsend(buf));
}

/*
 * Create a tpquery from text (no index - InvalidOid)
 */
Datum
to_tpquery_text(PG_FUNCTION_ARGS)
{
	text	*input_text = PG_GETARG_TEXT_PP(0);
	char	*query_text = text_to_cstring(input_text);
	TpQuery *result		= create_tpquery(query_text, InvalidOid);

	pfree(query_text);
	PG_RETURN_POINTER(result);
}

/*
 * Create a tpquery from text with index name (resolves to OID)
 */
Datum
to_tpquery_text_index(PG_FUNCTION_ARGS)
{
	text	*input_text = PG_GETARG_TEXT_PP(0);
	text	*index_text = PG_GETARG_TEXT_PP(1);
	char	*query_text = text_to_cstring(input_text);
	char	*index_name = text_to_cstring(index_text);
	TpQuery *result		= create_tpquery_from_name(query_text, index_name);

	pfree(query_text);
	pfree(index_name);
	PG_RETURN_POINTER(result);
}

/*
 * Find the first child index of a partitioned index via pg_inherits.
 * Returns InvalidOid if no children found.
 */
static Oid
find_first_child_index(Oid parent_index_oid)
{
	Relation	inhrel;
	SysScanDesc scan;
	HeapTuple	tuple;
	ScanKeyData skey;
	Oid			child_oid = InvalidOid;

	inhrel = table_open(InheritsRelationId, AccessShareLock);

	ScanKeyInit(
			&skey,
			Anum_pg_inherits_inhparent,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(parent_index_oid));

	scan = systable_beginscan(
			inhrel, InheritsParentIndexId, true, NULL, 1, &skey);

	tuple = systable_getnext(scan);
	if (tuple != NULL)
	{
		Form_pg_inherits inhform = (Form_pg_inherits)GETSTRUCT(tuple);
		child_oid				 = inhform->inhrelid;
	}

	systable_endscan(scan);
	table_close(inhrel, AccessShareLock);

	return child_oid;
}

/*
 * Find the first child BM25 index for an inheritance parent index.
 * This handles hypertables where child indexes don't have pg_inherits
 * relationship to the parent index - we find via the table hierarchy.
 *
 * Returns InvalidOid if no matching child index found.
 */
static Oid
find_first_child_bm25_index(Oid parent_index_oid, AttrNumber indexed_attnum)
{
	Relation	  inhrel;
	SysScanDesc	  scan;
	HeapTuple	  tuple;
	ScanKeyData	  skey;
	HeapTuple	  idx_tuple;
	Form_pg_index idx_form;
	Oid			  parent_table_oid;
	Oid			  bm25_am_oid;
	Oid			  result = InvalidOid;

	/* Look up BM25 access method OID */
	bm25_am_oid = get_am_oid("bm25", true);
	if (!OidIsValid(bm25_am_oid))
		return InvalidOid;

	/* Get the table this index is on */
	idx_tuple =
			SearchSysCache1(INDEXRELID, ObjectIdGetDatum(parent_index_oid));
	if (!HeapTupleIsValid(idx_tuple))
		return InvalidOid;
	idx_form		 = (Form_pg_index)GETSTRUCT(idx_tuple);
	parent_table_oid = idx_form->indrelid;
	ReleaseSysCache(idx_tuple);

	/* Find first child table */
	inhrel = table_open(InheritsRelationId, AccessShareLock);

	ScanKeyInit(
			&skey,
			Anum_pg_inherits_inhparent,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(parent_table_oid));

	scan = systable_beginscan(
			inhrel, InheritsParentIndexId, true, NULL, 1, &skey);

	while ((tuple = systable_getnext(scan)) != NULL && result == InvalidOid)
	{
		Form_pg_inherits inhform		 = (Form_pg_inherits)GETSTRUCT(tuple);
		Oid				 child_table_oid = inhform->inhrelid;
		Relation		 child_table;
		List			*child_indexes;
		ListCell		*lc;

		child_table	  = table_open(child_table_oid, AccessShareLock);
		child_indexes = RelationGetIndexList(child_table);

		foreach (lc, child_indexes)
		{
			Oid			  child_idx_oid = lfirst_oid(lc);
			HeapTuple	  child_idx_tuple;
			HeapTuple	  child_class_tuple;
			Form_pg_index child_idx_form;
			Form_pg_class child_class_form;

			child_idx_tuple = SearchSysCache1(
					INDEXRELID, ObjectIdGetDatum(child_idx_oid));
			if (!HeapTupleIsValid(child_idx_tuple))
				continue;

			child_idx_form = (Form_pg_index)GETSTRUCT(child_idx_tuple);

			/* Check it's on the same column */
			if (child_idx_form->indnatts < 1 ||
				child_idx_form->indkey.values[0] != indexed_attnum)
			{
				ReleaseSysCache(child_idx_tuple);
				continue;
			}

			/* Check it's a BM25 index */
			child_class_tuple =
					SearchSysCache1(RELOID, ObjectIdGetDatum(child_idx_oid));
			if (!HeapTupleIsValid(child_class_tuple))
			{
				ReleaseSysCache(child_idx_tuple);
				continue;
			}

			child_class_form = (Form_pg_class)GETSTRUCT(child_class_tuple);

			if (child_class_form->relam == bm25_am_oid)
			{
				result = child_idx_oid;
				ReleaseSysCache(child_class_tuple);
				ReleaseSysCache(child_idx_tuple);
				break;
			}

			ReleaseSysCache(child_class_tuple);
			ReleaseSysCache(child_idx_tuple);
		}

		list_free(child_indexes);
		table_close(child_table, AccessShareLock);
	}

	systable_endscan(scan);
	table_close(inhrel, AccessShareLock);

	return result;
}

/*
 * Helper: Validate query and get index
 * Returns opened index relation, sets index_oid_out if provided
 *
 * For partitioned indexes (created on partitioned tables), this function
 * finds the first child index to use for text config. The actual corpus
 * stats are aggregated from all children later.
 */
static Relation
validate_and_open_index(TpQuery *query, Oid *index_oid_out)
{
	Oid		 index_oid = get_tpquery_index_oid(query);
	Relation index_rel;
	char	 relkind;

	if (!OidIsValid(index_oid))
	{
		/* No index resolved - return ERROR */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text <@> tpquery operator requires index"),
				 errhint("Use to_tpquery(text, index_name) for standalone "
						 "scoring")));
	}

	/*
	 * Check if this is a partitioned index. Partitioned indexes don't have
	 * storage - they're just templates. We need to use a child index for
	 * text config, but aggregate stats from all children.
	 */
	relkind = get_rel_relkind(index_oid);
	if (relkind == RELKIND_PARTITIONED_INDEX)
	{
		Oid child_index_oid = find_first_child_index(index_oid);
		if (!OidIsValid(child_index_oid))
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("partitioned index has no child partitions"),
					 errdetail(
							 "The index \"%s\" is a partitioned index with "
							 "no partition indexes.",
							 get_rel_name(index_oid))));
		}
		/* Use the child index for text config access */
		index_rel = index_open(child_index_oid, AccessShareLock);
	}
	else
	{
		/* Open the index relation directly */
		index_rel = index_open(index_oid, AccessShareLock);
	}

	if (index_oid_out)
		*index_oid_out = index_oid;

	return index_rel;
}

/*
 * Helper: Calculate document length from tsvector
 */
static float4
calculate_doc_length(TSVector tsvector)
{
	WordEntry *entries		  = ARRPTR(tsvector);
	int		   doc_term_count = tsvector->size;
	float4	   doc_length	  = 0.0f;
	int		   i;

	for (i = 0; i < doc_term_count; i++)
	{
		int term_freq;
		if (entries[i].haspos)
			term_freq = (int32)POSDATALEN(tsvector, &entries[i]);
		else
			term_freq = 1;
		doc_length += term_freq;
	}

	return doc_length;
}

/*
 * Helper: Find term frequency in document
 */
static float4
find_term_frequency(
		TSVector tsvector, WordEntry *query_entry, char *query_lexeme)
{
	WordEntry *entries		  = ARRPTR(tsvector);
	char	  *lexemes_start  = STRPTR(tsvector);
	int		   doc_term_count = tsvector->size;
	int		   i;

	for (i = 0; i < doc_term_count; i++)
	{
		char *doc_lexeme = lexemes_start + entries[i].pos;
		if (entries[i].len == query_entry->len &&
			memcmp(doc_lexeme, query_lexeme, entries[i].len) == 0)
		{
			if (entries[i].haspos)
				return (int32)POSDATALEN(tsvector, &entries[i]);
			else
				return 1;
		}
	}

	return 0.0f; /* Term not found */
}

/*
 * Helper: Calculate BM25 score for a single term
 */
static float4
calculate_term_score(
		float4 tf,
		float4 idf,
		float4 doc_length,
		float4 avg_doc_len,
		int	   query_freq)
{
	double numerator_d;
	double denominator_d;
	float4 term_score;

	/* BM25 formula with default k1=1.2, b=0.75 */
	numerator_d = (double)tf * (1.2 + 1.0);

	if (avg_doc_len > 0.0f)
	{
		denominator_d = (double)tf + 1.2 * (1.0 - 0.75 +
											0.75 * ((double)doc_length /
													(double)avg_doc_len));
	}
	else
	{
		denominator_d = (double)tf + 1.2;
	}

	term_score = (float4)((double)idf * (numerator_d / denominator_d) *
						  (double)query_freq);

	return term_score;
}

/*
 * BM25 scoring function for text <@> tpquery operations
 *
 * This operator is called per-row when scoring documents, so we use fn_extra
 * to cache IDF values across rows. Without caching, each row would require
 * opening all segments for each query term - catastrophic for large indexes.
 */
Datum
text_tpquery_score(PG_FUNCTION_ARGS)
{
	text	*text_arg	= PG_GETARG_TEXT_PP(0);
	TpQuery *query		= (TpQuery *)PG_GETARG_POINTER(1);
	char	*query_text = get_tpquery_text(query);
	Oid		 index_oid;

	Relation		   index_rel = NULL;
	TpIndexMetaPage	   metap	 = NULL;
	Oid				   text_config_oid;
	Datum			   tsvector_datum;
	TSVector		   tsvector;
	Datum			   query_tsvector_datum;
	TSVector		   query_tsvector;
	WordEntry		  *query_entries;
	char			  *query_lexemes_start;
	TpLocalIndexState *index_state;
	float4			   avg_doc_len;
	int32			   total_docs;
	int64			   total_len;
	float8			   result = 0.0;
	int				   q_i;
	float4			   doc_length;
	int				   query_term_count;
	QueryScoreCache	  *cache;
	BlockNumber		   first_segment;
	BlockNumber		   level_heads[TP_MAX_LEVELS];
	bool			   is_partitioned;
	AttrNumber		   indexed_attnum = InvalidAttrNumber;

	/* Get index OID from query */
	index_oid = get_tpquery_index_oid(query);

	/*
	 * Validate query and open index. For partitioned indexes, this opens
	 * the first child index for text config access.
	 */
	index_rel = validate_and_open_index(query, &index_oid);

	/* Check if this is a partitioned index */
	is_partitioned = (get_rel_relkind(index_oid) == RELKIND_PARTITIONED_INDEX);

	/*
	 * Get the indexed attribute number for child index matching.
	 * This is needed for both partitioned indexes and inheritance tables.
	 */
	{
		HeapTuple	  idx_tuple;
		Form_pg_index idx_form;
		Oid			  lookup_oid;

		/*
		 * For partitioned indexes, use the parent. For regular indexes,
		 * use the index itself.
		 */
		lookup_oid = index_oid;
		idx_tuple  = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(lookup_oid));
		if (HeapTupleIsValid(idx_tuple))
		{
			idx_form = (Form_pg_index)GETSTRUCT(idx_tuple);
			if (idx_form->indnatts >= 1)
				indexed_attnum = idx_form->indkey.values[0];
			ReleaseSysCache(idx_tuple);
		}
	}

	PG_TRY();
	{
		/* Get the metapage to extract text_config_oid */
		metap			= tp_get_metapage(index_rel);
		text_config_oid = metap->text_config_oid;
		first_segment	= metap->level_heads[0];
		for (int i = 0; i < TP_MAX_LEVELS; i++)
			level_heads[i] = metap->level_heads[i];

		/*
		 * Get corpus statistics. For partitioned indexes or inheritance
		 * parents, use the first child's stats as an approximation.
		 */
		if (is_partitioned)
		{
			Oid child_index_oid = RelationGetRelid(index_rel);

			/*
			 * Partitioned indexes have no storage. Use the first child's
			 * stats as an approximation. The child index is the one we
			 * already opened for text config access.
			 */
			index_state = tp_get_local_index_state(child_index_oid);
			if (!index_state)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("could not get index state for partition "
								"index OID %u",
								child_index_oid)));
			}

			total_docs	= index_state->shared->total_docs;
			total_len	= index_state->shared->total_len;
			avg_doc_len = total_docs > 0 ? (float4)((double)total_len /
													(double)total_docs)
										 : 0.0f;
		}
		else
		{
			/* Get index state for corpus statistics */
			index_state = tp_get_local_index_state(index_oid);
			if (!index_state)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("could not get index state for index OID %u",
								index_oid)));
			}

			total_docs = index_state->shared->total_docs;
			total_len  = index_state->shared->total_len;

			/*
			 * If the index has no documents, check if this is an
			 * inheritance parent (e.g., hypertable) and use the first
			 * child's stats as an approximation.
			 */
			if (total_docs == 0 && indexed_attnum != InvalidAttrNumber)
			{
				Oid first_child_idx =
						find_first_child_bm25_index(index_oid, indexed_attnum);

				if (OidIsValid(first_child_idx))
				{
					TpLocalIndexState *child_state = tp_get_local_index_state(
							first_child_idx);
					if (child_state && child_state->shared)
					{
						index_state = child_state;
						total_docs	= child_state->shared->total_docs;
						total_len	= child_state->shared->total_len;
					}
				}
			}

			avg_doc_len = total_docs > 0 ? (float4)((double)total_len /
													(double)total_docs)
										 : 0.0f;
		}

		/*
		 * Get or initialize the IDF cache. The cache is stored in fn_extra
		 * and persists for the duration of the query. We invalidate it if
		 * the index state changes (new segments, new documents).
		 */
		cache = (QueryScoreCache *)fcinfo->flinfo->fn_extra;
		if (!cache_is_valid(cache, index_oid, first_segment, total_docs))
		{
			/* Allocate new cache in function memory context */
			cache = (QueryScoreCache *)MemoryContextAllocZero(
					fcinfo->flinfo->fn_mcxt, sizeof(QueryScoreCache));
			cache->index_oid		 = index_oid;
			cache->first_segment	 = first_segment;
			cache->total_docs		 = total_docs;
			cache->avg_doc_len		 = avg_doc_len;
			cache->num_terms		 = 0;
			fcinfo->flinfo->fn_extra = cache;
		}

		/* Tokenize the document text using the index's text configuration */
		tsvector_datum = DirectFunctionCall2Coll(
				to_tsvector_byid,
				InvalidOid, /* collation */
				ObjectIdGetDatum(text_config_oid),
				PointerGetDatum(text_arg));

		tsvector = DatumGetTSVector(tsvector_datum);

		/* Tokenize the query text to get query terms */
		query_tsvector_datum = DirectFunctionCall2Coll(
				to_tsvector_byid,
				InvalidOid,
				ObjectIdGetDatum(text_config_oid),
				PointerGetDatum(cstring_to_text(query_text)));

		query_tsvector		= DatumGetTSVector(query_tsvector_datum);
		query_entries		= ARRPTR(query_tsvector);
		query_lexemes_start = STRPTR(query_tsvector);

		/* Calculate document length and prepare term data */
		doc_length		 = calculate_doc_length(tsvector);
		query_term_count = query_tsvector->size;

		/* Calculate BM25 score for each query term */
		for (q_i = 0; q_i < query_term_count; q_i++)
		{
			char *query_lexeme_raw = query_lexemes_start +
									 query_entries[q_i].pos;
			int			   lexeme_len = query_entries[q_i].len;
			char		  *query_lexeme;
			TpPostingList *posting_list;
			float4		   idf;
			float4		   tf; /* term frequency in document */
			float4		   term_score;
			int			   query_freq;

			/* Create properly null-terminated string from tsvector lexeme */
			query_lexeme = palloc(lexeme_len + 1);
			memcpy(query_lexeme, query_lexeme_raw, lexeme_len);
			query_lexeme[lexeme_len] = '\0';

			if (query_entries[q_i].haspos)
				query_freq = (int32)
						POSDATALEN(query_tsvector, &query_entries[q_i]);
			else
				query_freq = 1;

			/* Find term frequency in the document */
			tf = find_term_frequency(
					tsvector, &query_entries[q_i], query_lexeme);

			if (tf == 0.0f)
			{
				pfree(query_lexeme);
				continue; /* Query term not in document */
			}

			/*
			 * Get IDF from cache if available. The cache avoids repeated
			 * segment opens which was causing catastrophic performance
			 * (opening all segments for each term for each row).
			 */
			{
				uint32 cached_doc_freq = 0;
				idf = lookup_cached_idf(cache, query_lexeme, &cached_doc_freq);

				if (idf < 0.0f)
				{
					/* Cache miss - calculate IDF and cache it */
					uint32 unified_doc_freq	 = 0;
					uint32 memtable_doc_freq = 0;
					uint32 segment_doc_freq	 = 0;

					posting_list =
							tp_get_posting_list(index_state, query_lexeme);
					if (posting_list && posting_list->doc_count > 0)
						memtable_doc_freq = posting_list->doc_count;

					/* Get doc_freq from all segment levels */
					for (int level = 0; level < TP_MAX_LEVELS; level++)
					{
						if (level_heads[level] != InvalidBlockNumber)
						{
							segment_doc_freq += tp_segment_get_doc_freq(
									index_rel,
									level_heads[level],
									query_lexeme);
						}
					}

					unified_doc_freq = memtable_doc_freq + segment_doc_freq;
					if (unified_doc_freq == 0)
					{
						pfree(query_lexeme);
						continue;
					}

					/* Calculate IDF and cache it */
					idf = tp_calculate_idf(unified_doc_freq, total_docs);
					cache_term_idf(cache, query_lexeme, unified_doc_freq, idf);
				}
			}

			/* Calculate BM25 term score */
			term_score = calculate_term_score(
					tf, idf, doc_length, avg_doc_len, query_freq);

			/* Accumulate the score */
			result += term_score;

			/* Free the null-terminated string copy */
			pfree(query_lexeme);
		}

		/* Clean up */
		pfree(metap);
		metap = NULL;
		index_close(index_rel, AccessShareLock);
		index_rel = NULL;
	}
	PG_CATCH();
	{
		/* Clean up on error */
		if (metap)
			pfree(metap);
		if (index_rel)
			index_close(index_rel, AccessShareLock);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Return negative score for PostgreSQL ASC ordering compatibility */
	PG_RETURN_FLOAT8((result > 0) ? -result : result);
}

/*
 * tpquery equality function
 */
Datum
tpquery_eq(PG_FUNCTION_ARGS)
{
	TpQuery *a = (TpQuery *)PG_GETARG_POINTER(0);
	TpQuery *b = (TpQuery *)PG_GETARG_POINTER(1);

	/* Compare index OIDs */
	if (a->index_oid != b->index_oid)
		PG_RETURN_BOOL(false);

	/* Compare query text lengths */
	if (a->query_text_len != b->query_text_len)
		PG_RETURN_BOOL(false);

	/* Compare query texts */
	{
		char *query_text_a = get_tpquery_text(a);
		char *query_text_b = get_tpquery_text(b);

		if (strcmp(query_text_a, query_text_b) != 0)
			PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}

/*
 * Utility function to create a tpquery with resolved index OID
 */
TpQuery *
create_tpquery(const char *query_text, Oid index_oid)
{
	TpQuery *result;
	int		 query_text_len = strlen(query_text);
	int		 total_size;

	/* Calculate total size: header + version + oid + text_len + text + null */
	total_size = offsetof(TpQuery, data) + query_text_len + 1;

	result = (TpQuery *)palloc0(total_size);
	SET_VARSIZE(result, total_size);
	result->version		   = TPQUERY_VERSION;
	result->index_oid	   = index_oid;
	result->query_text_len = query_text_len;

	/* Copy query text */
	memcpy(result->data, query_text, query_text_len);
	result->data[query_text_len] = '\0';

	return result;
}

/*
 * Create a tpquery from index name (resolves name to OID)
 *
 * Partitioned indexes are allowed - they will be resolved to the
 * appropriate partition index at scan time.
 */
TpQuery *
create_tpquery_from_name(const char *query_text, const char *index_name)
{
	Oid index_oid = InvalidOid;

	if (index_name != NULL)
	{
		index_oid = tp_resolve_index_name_shared(index_name);
		if (!OidIsValid(index_oid))
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("index \"%s\" does not exist", index_name)));
		}
	}

	return create_tpquery(query_text, index_oid);
}

/*
 * Get index OID from tpquery (returns InvalidOid if unresolved)
 */
Oid
get_tpquery_index_oid(TpQuery *tpquery)
{
	return tpquery->index_oid;
}

/*
 * Get query text from tpquery
 */
char *
get_tpquery_text(TpQuery *tpquery)
{
	return TPQUERY_TEXT_PTR(tpquery);
}

/*
 * Check if tpquery has a resolved index
 */
bool
tpquery_has_index(TpQuery *tpquery)
{
	return OidIsValid(tpquery->index_oid);
}

/*
 * Returns a hardcoded positive cost estimate for planning purposes.
 */
Datum
tp_distance(PG_FUNCTION_ARGS)
{
	/* Arguments are required by function signature but not used for costing */
	(void)PG_GETARG_TEXT_PP(0); /* text argument */
	(void)PG_GETARG_POINTER(1); /* tpquery argument */

	/*
	 * Return hardcoded cost estimate for planning.
	 * Value indicates this operation has some cost but is generally efficient.
	 */
	PG_RETURN_FLOAT8(1.0);
}

/*
 * Scoring function for text <@> text operations.
 *
 * This function should never actually be called because the planner hook
 * transforms all `text <@> text` expressions to `text <@> bm25query`.
 * We keep this as a fallback that errors with a helpful message.
 */
Datum
text_text_score(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no BM25 index found for text <@> text expression"),
			 errdetail("Create a BM25 index on the column or use ORDER BY."),
			 errhint("SELECT col <@> to_bm25query('q', 'idx') AS score")));

	PG_RETURN_NULL(); /* never reached */
}
