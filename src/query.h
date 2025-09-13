#pragma once

#include <postgres.h>

#include <fmgr.h>

/*
 * tpquery data type structure
 * Represents a query for BM25 scoring with optional index name
 */
typedef struct TpQuery
{
	int32 vl_len_;					   /* varlena header (must be first) */
	int32 index_name_len;			   /* length of index name (0 if none) */
	int32 query_text_len;			   /* length of query text */
	char  data[FLEXIBLE_ARRAY_MEMBER]; /* payload: index name + query text */
} TpQuery;

/* Macros for accessing tpquery variable-length data */
#define TPQUERY_INDEX_NAME_PTR(x) (((TpQuery *)(x))->data)
#define TPQUERY_TEXT_PTR(x)                  \
	((((TpQuery *)(x))->index_name_len == 0) \
			 ? (((TpQuery *)(x))->data)      \
			 : (((TpQuery *)(x))->data +     \
				MAXALIGN(((TpQuery *)(x))->index_name_len + 1)))

/* Function declarations */
Datum tpquery_in(PG_FUNCTION_ARGS);
Datum tpquery_out(PG_FUNCTION_ARGS);
Datum tpquery_recv(PG_FUNCTION_ARGS);
Datum tpquery_send(PG_FUNCTION_ARGS);

/* Constructor functions */
Datum to_tpquery_text(PG_FUNCTION_ARGS);
Datum to_tpquery_text_index(PG_FUNCTION_ARGS);

/* Operator functions */
Datum text_tpquery_score(PG_FUNCTION_ARGS);
Datum tpvector_tpquery_score(PG_FUNCTION_ARGS);
Datum tpquery_eq(PG_FUNCTION_ARGS);

/* Utility functions */
TpQuery *create_tpquery(const char *query_text, const char *index_name);
char	*get_tpquery_index_name(TpQuery *tpquery);
char	*get_tpquery_text(TpQuery *tpquery);
bool	 tpquery_has_index_name(TpQuery *tpquery);
