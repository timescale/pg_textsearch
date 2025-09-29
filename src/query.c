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
#include "memtable.h"
#include "metapage.h"
#include "operator.h"
#include "posting.h"
#include "query.h"
#include "state.h"
#include "vector.h"

/* Local helper functions */

PG_FUNCTION_INFO_V1(tpquery_in);
PG_FUNCTION_INFO_V1(tpquery_out);
PG_FUNCTION_INFO_V1(tpquery_recv);
PG_FUNCTION_INFO_V1(tpquery_send);
PG_FUNCTION_INFO_V1(to_tpquery_text);
PG_FUNCTION_INFO_V1(to_tpquery_text_index);
PG_FUNCTION_INFO_V1(text_tpquery_score);
PG_FUNCTION_INFO_V1(tp_distance);
PG_FUNCTION_INFO_V1(tpquery_eq);

/*
 * tpquery input function
 * Format: "query_text" (simple cast from text)
 * Index names not supported in text cast - use to_tpquery() functions instead
 */
Datum
tpquery_in(PG_FUNCTION_ARGS)
{
	char	*str = PG_GETARG_CSTRING(0);
	TpQuery *result;

	/* Simple format: just the query text, no special syntax */
	result = create_tpquery(str, NULL);

	PG_RETURN_POINTER(result);
}

/*
 * tpquery output function
 */
Datum
tpquery_out(PG_FUNCTION_ARGS)
{
	TpQuery	  *tpquery = (TpQuery *)PG_GETARG_POINTER(0);
	StringInfo str	   = makeStringInfo();

	if (tpquery->index_name_len > 0)
	{
		/* Format with index name: "index_name:{query_text}" */
		char *index_name = get_tpquery_index_name(tpquery);
		char *query_text = get_tpquery_text(tpquery);

		appendStringInfo(str, "%s:{%s}", index_name, query_text);
	}
	else
	{
		/* Format without index name: just the query text */
		char *query_text = get_tpquery_text(tpquery);

		appendStringInfo(str, "%s", query_text);
	}

	PG_RETURN_CSTRING(str->data);
}

/*
 * tpquery receive function (binary input)
 */
Datum
tpquery_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
	TpQuery	  *result;
	int32	   index_name_len;
	int32	   query_text_len;
	char	  *index_name = NULL;
	char	  *query_text;

	index_name_len = pq_getmsgint(buf, sizeof(int32));
	query_text_len = pq_getmsgint(buf, sizeof(int32));

	/* Validate lengths to prevent unbounded memory allocation */
	if (index_name_len < 0 || index_name_len > 1000000) /* 1MB limit */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid index name length: %d", index_name_len)));

	if (query_text_len < 0 || query_text_len > 1000000) /* 1MB limit */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid query text length: %d", query_text_len)));

	if (index_name_len > 0)
	{
		index_name = palloc(index_name_len + 1);
		pq_copymsgbytes(buf, index_name, index_name_len);
		index_name[index_name_len] = '\0';
	}

	query_text = palloc(query_text_len + 1);
	pq_copymsgbytes(buf, query_text, query_text_len);
	query_text[query_text_len] = '\0';

	result = create_tpquery(query_text, index_name);

	if (index_name)
		pfree(index_name);
	pfree(query_text);

	PG_RETURN_POINTER(result);
}

/*
 * tpquery send function (binary output)
 */
Datum
tpquery_send(PG_FUNCTION_ARGS)
{
	TpQuery	  *tpquery = (TpQuery *)PG_GETARG_POINTER(0);
	StringInfo buf	   = makeStringInfo();

	pq_sendint32(buf, tpquery->index_name_len);
	pq_sendint32(buf, tpquery->query_text_len);

	if (tpquery->index_name_len > 0)
	{
		char *index_name = get_tpquery_index_name(tpquery);
		pq_sendbytes(buf, index_name, tpquery->index_name_len);
	}

	{
		char *query_text = get_tpquery_text(tpquery);
		pq_sendbytes(buf, query_text, tpquery->query_text_len);
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(buf));
}

/*
 * Create a tpquery from text (no index name)
 */
Datum
to_tpquery_text(PG_FUNCTION_ARGS)
{
	text	*input_text = PG_GETARG_TEXT_PP(0);
	char	*query_text = text_to_cstring(input_text);
	TpQuery *result		= create_tpquery(query_text, NULL);

	pfree(query_text);
	PG_RETURN_POINTER(result);
}

/*
 * Create a tpquery from text with index name
 */
Datum
to_tpquery_text_index(PG_FUNCTION_ARGS)
{
	text	*input_text = PG_GETARG_TEXT_PP(0);
	text	*index_text = PG_GETARG_TEXT_PP(1);
	char	*query_text = text_to_cstring(input_text);
	char	*index_name = text_to_cstring(index_text);
	TpQuery *result		= create_tpquery(query_text, index_name);

	pfree(query_text);
	pfree(index_name);
	PG_RETURN_POINTER(result);
}

/*
 * Helper: Validate query and get index
 */
static Relation
validate_and_open_index(TpQuery *query, char **index_name_out)
{
	char	*index_name = get_tpquery_index_name(query);
	Oid		 index_oid;
	Relation index_rel;

	if (!index_name)
	{
		/* No index name - return ERROR as requested */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text <@> tpquery operator requires index name"),
				 errhint("Use to_tpquery(text, index_name) for standalone "
						 "scoring")));
	}

	/* Get the index OID from index name */
	index_oid = tp_resolve_index_name_shared(index_name);

	if (!OidIsValid(index_oid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" not found", index_name)));
	}

	/* Open the index relation */
	index_rel = index_open(index_oid, AccessShareLock);

	if (index_name_out)
		*index_name_out = index_name;

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
 */
Datum
text_tpquery_score(PG_FUNCTION_ARGS)
{
	text			  *text_arg = PG_GETARG_TEXT_PP(0);
	TpQuery			  *query	= (TpQuery *)PG_GETARG_POINTER(1);
	char			  *index_name;
	char			  *query_text = get_tpquery_text(query);
	Oid				   index_oid;
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

	/* Validate query and open index */
	index_rel = validate_and_open_index(query, &index_name);
	index_oid = RelationGetRelid(index_rel);

	PG_TRY();
	{
		/* Get the metapage to extract text_config_oid */
		metap			= tp_get_metapage(index_rel);
		text_config_oid = metap->text_config_oid;

		/* Get index state for corpus statistics */
		index_state = tp_get_local_index_state(index_oid);
		if (!index_state)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not get index state for \"%s\"",
							index_name)));
		}

		total_docs	= index_state->shared->total_docs;
		avg_doc_len = total_docs > 0
							? (float4)(index_state->shared->total_len /
									   (double)total_docs)
							: 0.0f;

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
				continue; /* Query term not in document */
			}

			/* Get posting list for this term */
			posting_list = tp_get_posting_list(index_state, query_lexeme);
			if (!posting_list || posting_list->doc_count == 0)
			{
				continue; /* Term not in index, skip */
			}

			/* Calculate IDF with epsilon handling */
			{
				TpMemtable *memtable = get_memtable(index_state);
				float8		avg_idf;

				if (!memtable)
					elog(ERROR, "Cannot get memtable - index state corrupted");

				if (memtable->total_terms == 0)
					elog(ERROR, "Invalid index state: total_terms is zero");

				avg_idf = index_state->shared->idf_sum / memtable->total_terms;
				idf		= tp_calculate_idf_with_epsilon(
						posting_list->doc_count, total_docs, avg_idf);
				elog(DEBUG1,
					 "Query scoring: term='%s', doc_freq=%d, "
					 "total_docs=%d, idf_sum=%.6f, total_terms=%d, "
					 "avg_idf=%.6f, final_idf=%.6f",
					 query_lexeme,
					 posting_list->doc_count,
					 total_docs,
					 index_state->shared->idf_sum,
					 memtable->total_terms,
					 avg_idf,
					 idf);
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

	/* Compare sizes first */
	if (VARSIZE(a) != VARSIZE(b))
		PG_RETURN_BOOL(false);

	/* Compare index name lengths */
	if (a->index_name_len != b->index_name_len)
		PG_RETURN_BOOL(false);

	/* Compare query text lengths */
	if (a->query_text_len != b->query_text_len)
		PG_RETURN_BOOL(false);

	/* Compare index names if present */
	if (a->index_name_len > 0)
	{
		char *index_name_a = get_tpquery_index_name(a);
		char *index_name_b = get_tpquery_index_name(b);

		if (strcmp(index_name_a, index_name_b) != 0)
			PG_RETURN_BOOL(false);
	}

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
 * Utility function to create a tpquery
 */
TpQuery *
create_tpquery(const char *query_text, const char *index_name)
{
	TpQuery *result;
	int		 query_text_len = strlen(query_text);
	int		 index_name_len = index_name ? strlen(index_name) : 0;
	int		 total_size;
	char	*data_ptr;

	/* Calculate total size */
	total_size = VARHDRSZ + sizeof(int32) +
				 sizeof(int32) + /* index_name_len, query_text_len */
				 (index_name_len > 0 ? MAXALIGN(index_name_len + 1) : 0) +
				 query_text_len + 1;

	result = (TpQuery *)palloc0(total_size);
	SET_VARSIZE(result, total_size);
	result->index_name_len = index_name_len;
	result->query_text_len = query_text_len;

	data_ptr = result->data;

	/* Copy index name if present */
	if (index_name_len > 0)
	{
		memcpy(data_ptr, index_name, index_name_len);
		data_ptr[index_name_len] = '\0';
		data_ptr += MAXALIGN(index_name_len + 1);
	}

	/* Copy query text */
	memcpy(data_ptr, query_text, query_text_len);
	data_ptr[query_text_len] = '\0';

	return result;
}

/*
 * Get index name from tpquery (returns NULL if none)
 */
char *
get_tpquery_index_name(TpQuery *tpquery)
{
	if (tpquery->index_name_len == 0)
		return NULL;

	return TPQUERY_INDEX_NAME_PTR(tpquery);
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
 * Check if tpquery has an index name
 */
bool
tpquery_has_index_name(TpQuery *tpquery)
{
	return tpquery->index_name_len > 0;
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
