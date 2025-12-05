#include <postgres.h>

#include <access/relation.h>
#include <access/xact.h>
#include <catalog/namespace.h>
#include <lib/stringinfo.h>
#include <libpq/pqformat.h>
#include <nodes/pg_list.h>
#include <nodes/value.h>
#include <tsearch/ts_type.h>
#include <utils/builtins.h>
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
 */
static void
cache_term_idf(
		QueryScoreCache *cache, const char *term, uint32 doc_freq, float4 idf)
{
	if (!cache || cache->num_terms >= MAX_CACHED_TERMS)
		return;

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
PG_FUNCTION_INFO_V1(tp_distance_text_text);
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
 * Binary format: index_oid (4 bytes) + query_text_len (4 bytes) + query_text
 */
Datum
tpquery_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
	TpQuery	  *result;
	Oid		   index_oid;
	int32	   query_text_len;
	char	  *query_text;

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
 * Binary format: index_oid (4 bytes) + query_text_len (4 bytes) + query_text
 */
Datum
tpquery_send(PG_FUNCTION_ARGS)
{
	TpQuery	  *tpquery = (TpQuery *)PG_GETARG_POINTER(0);
	StringInfo buf	   = makeStringInfo();
	char	  *query_text;

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
 * Helper: Validate query and get index
 * Returns opened index relation, sets index_oid_out if provided
 *
 * For partitioned indexes (created on partitioned tables), this function
 * will error out because partitioned indexes don't have storage - each
 * partition has its own index. The index scan path handles this correctly
 * by using per-partition indexes, but the standalone operator cannot.
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
	 * storage - they're just templates. Each partition has its own actual
	 * index. We can't use a partitioned index for standalone scoring.
	 */
	relkind = get_rel_relkind(index_oid);
	if (relkind == RELKIND_PARTITIONED_INDEX)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BM25 scoring on partitioned tables requires "
						"ORDER BY clause"),
				 errdetail(
						 "The index \"%s\" is a partitioned index. "
						 "Partitioned indexes don't have storage.",
						 get_rel_name(index_oid)),
				 errhint("Use ORDER BY content <@> to_bm25query(...) to "
						 "score documents in partitioned tables.")));
	}

	/* Open the index relation */
	index_rel = index_open(index_oid, AccessShareLock);

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
	float8			   result = 0.0;
	int				   q_i;
	float4			   doc_length;
	int				   query_term_count;
	QueryScoreCache	  *cache;
	BlockNumber		   first_segment;
	BlockNumber		   level_heads[TP_MAX_LEVELS];

	/* Get index OID from query */
	index_oid = get_tpquery_index_oid(query);

	/*
	 * For partitioned indexes, we cannot open the index directly (no storage).
	 * Instead, use the score cache populated by tp_gettuple during index scan.
	 * This works because the ORDER BY clause triggers the index scan which
	 * computes and caches scores before the projection phase.
	 */
	if (OidIsValid(index_oid) &&
		get_rel_relkind(index_oid) == RELKIND_PARTITIONED_INDEX)
	{
		ItemPointer current_ctid	  = tp_get_current_row_ctid();
		Oid			current_table_oid = tp_get_current_row_table_oid();

		if (current_ctid && ItemPointerIsValid(current_ctid))
		{
			uint32 query_hash = tp_hash_query(index_oid, query_text);
			bool   found	  = false;
			float4 cached	  = tp_get_cached_score(
					current_ctid, current_table_oid, query_hash, &found);

			if (found)
			{
				elog(DEBUG2,
					 "text_tpquery_score: using cached score %.4f for "
					 "partitioned index (table %u)",
					 cached,
					 current_table_oid);
				PG_RETURN_FLOAT8((float8)cached);
			}
		}

		/*
		 * No cached score found for partitioned index. This happens when
		 * using the operator without ORDER BY, which doesn't trigger index
		 * scan. Return a clear error.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BM25 scoring on partitioned tables requires "
						"ORDER BY clause"),
				 errdetail(
						 "The index \"%s\" is a partitioned index. "
						 "Partitioned indexes don't have storage.",
						 get_rel_name(index_oid)),
				 errhint("Use ORDER BY content <@> to_bm25query(...) to "
						 "score documents in partitioned tables.")));
	}

	/* Validate query and open index (for non-partitioned indexes) */
	index_rel = validate_and_open_index(query, &index_oid);

	PG_TRY();
	{
		/* Get the metapage to extract text_config_oid */
		metap			= tp_get_metapage(index_rel);
		text_config_oid = metap->text_config_oid;
		first_segment	= metap->level_heads[0];
		for (int i = 0; i < TP_MAX_LEVELS; i++)
			level_heads[i] = metap->level_heads[i];

		/* Get index state for corpus statistics */
		index_state = tp_get_local_index_state(index_oid);
		if (!index_state)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not get index state for index OID %u",
							index_oid)));
		}

		total_docs	= index_state->shared->total_docs;
		avg_doc_len = total_docs > 0
							? (float4)(index_state->shared->total_len /
									   (double)total_docs)
							: 0.0f;

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

	/* Calculate total size: header + oid + text_len + text + null */
	total_size = offsetof(TpQuery, data) + query_text_len + 1;

	result = (TpQuery *)palloc0(total_size);
	SET_VARSIZE(result, total_size);
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
 * BM25 scoring function for text <@> text operations
 *
 * This function is called when evaluating text <@> 'query' expressions.
 * For index-ordered scans, the score was already computed by the index AM
 * and cached. We retrieve the cached score here.
 *
 * For non-index-scan contexts (e.g., WHERE clause without ORDER BY),
 * we return an error since we don't know which index to use.
 */
Datum
text_text_score(PG_FUNCTION_ARGS)
{
	ItemPointer current_ctid	  = tp_get_current_row_ctid();
	Oid			current_table_oid = tp_get_current_row_table_oid();
	bool		hash_valid		  = false;
	uint32		query_hash		  = tp_get_current_query_hash(&hash_valid);

	/*
	 * During an index-ordered scan, the index AM computes and caches scores.
	 * Look up the cached score using the current row's CTID and query hash.
	 */
	if (current_ctid && ItemPointerIsValid(current_ctid) && hash_valid)
	{
		bool   found  = false;
		float4 cached = tp_get_cached_score(
				current_ctid, current_table_oid, query_hash, &found);

		if (found)
		{
			elog(DEBUG2, "text_text_score: using cached score %.4f", cached);
			PG_RETURN_FLOAT8((float8)cached);
		}
	}

	/*
	 * No cached score found. This happens when using the operator outside
	 * of an index-ordered scan context.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("text <@> text operator requires ORDER BY clause"),
			 errhint("Use ORDER BY content <@> 'query' to enable BM25 "
					 "scoring, or use to_bm25query('query', 'index_name') "
					 "to specify an index explicitly.")));

	PG_RETURN_NULL(); /* never reached */
}

/*
 * Distance function for text <@> text operations (FUNCTION 8 in opclass)
 *
 * Returns a hardcoded cost estimate for planning purposes.
 * Like tp_distance, this is used for path costing - the actual
 * BM25 scores are computed by the index AM during ordered scans.
 */
Datum
tp_distance_text_text(PG_FUNCTION_ARGS)
{
	/* Arguments are required by function signature but not used for costing */
	(void)PG_GETARG_TEXT_PP(0); /* text column argument */
	(void)PG_GETARG_TEXT_PP(1); /* text query argument */

	/*
	 * Return hardcoded cost estimate for planning.
	 * Value indicates this operation has some cost but is generally efficient.
	 */
	PG_RETURN_FLOAT8(1.0);
}
