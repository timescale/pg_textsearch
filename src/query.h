#pragma once

#include <postgres.h>

#include <fmgr.h>

/*
 * tpquery data type structure
 * Represents a query for BM25 scoring with optional index reference
 *
 * The index can be specified by OID (resolved at creation time) or left
 * unresolved (InvalidOid) for later resolution by planner hooks.
 */
typedef struct TpQuery
{
	int32 vl_len_;					   /* varlena header (must be first) */
	Oid	  index_oid;				   /* resolved index OID (InvalidOid if
										* unresolved) */
	int32 query_text_len;			   /* length of query text */
	char  data[FLEXIBLE_ARRAY_MEMBER]; /* payload: query text only */
} TpQuery;

/* Macro for accessing query text */
#define TPQUERY_TEXT_PTR(x) (((TpQuery *)(x))->data)

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
Datum tp_distance(PG_FUNCTION_ARGS);
Datum tp_distance_text_text(PG_FUNCTION_ARGS);
Datum tpquery_eq(PG_FUNCTION_ARGS);

/* Utility functions */
TpQuery *create_tpquery(const char *query_text, Oid index_oid);
TpQuery		  *
create_tpquery_from_name(const char *query_text, const char *index_name);
Oid	  get_tpquery_index_oid(TpQuery *tpquery);
char *get_tpquery_text(TpQuery *tpquery);
bool  tpquery_has_index(TpQuery *tpquery);
