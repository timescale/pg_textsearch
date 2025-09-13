#include <postgres.h>

#include <catalog/namespace.h>
#include <lib/stringinfo.h>
#include <libpq/pqformat.h>
#include <tsearch/ts_type.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/syscache.h>
#include <varatt.h>

#include "constants.h"
#include "index.h"
#include "memtable.h"
#include "query.h"
#include "vector.h"

PG_FUNCTION_INFO_V1(tpquery_in);
PG_FUNCTION_INFO_V1(tpquery_out);
PG_FUNCTION_INFO_V1(tpquery_recv);
PG_FUNCTION_INFO_V1(tpquery_send);
PG_FUNCTION_INFO_V1(to_tpquery_text);
PG_FUNCTION_INFO_V1(to_tpquery_text_index);
PG_FUNCTION_INFO_V1(text_tpquery_score);
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
 * BM25 scoring function for text <@> tpquery operations
 */
Datum
text_tpquery_score(PG_FUNCTION_ARGS)
{
	text	*document_text = PG_GETARG_TEXT_PP(0);
	TpQuery *query		   = (TpQuery *)PG_GETARG_POINTER(1);
	char	*query_text	   = get_tpquery_text(query);
	char	*index_name	   = get_tpquery_index_name(query);
	float8	 result;

	if (!index_name)
	{
		/* No index name - this should only happen in index scan context */
		ereport(WARNING,
				(errmsg("text <@> tpquery operator called without index name "
						"- returning 0.0"),
				 errhint("Use to_tpquery(text, index_name) for standalone "
						 "scoring")));
		PG_RETURN_FLOAT8(0.0);
	}

	/* Create document vector and query vector, then score */
	{
		Datum doc_vector_datum = DirectFunctionCall2(
				to_tpvector,
				PointerGetDatum(document_text),
				CStringGetTextDatum(index_name));
		TpVector *doc_vector = (TpVector *)DatumGetPointer(doc_vector_datum);

		Datum query_vector_datum = DirectFunctionCall2(
				to_tpvector,
				CStringGetTextDatum(query_text),
				CStringGetTextDatum(index_name));
		TpVector *query_vector = (TpVector *)DatumGetPointer(
				query_vector_datum);

		result = DatumGetFloat8(DirectFunctionCall2(
				tpvector_score,
				PointerGetDatum(doc_vector),
				PointerGetDatum(query_vector)));
	}

	PG_RETURN_FLOAT8(result);
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
