#pragma once

#include <postgres.h>

#include <fmgr.h>

/*
 * Term frequency entry for tpvector format
 * Stores variable-length lexemes with their frequencies
 */
typedef struct TpVectorEntry
{
	int32 frequency;					 /* Term frequency in document */
	int32 lexeme_len;					 /* Length of lexeme string */
	char  lexeme[FLEXIBLE_ARRAY_MEMBER]; /* Variable-length lexeme string */
} TpVectorEntry;

/* tpvector data type structure */
typedef struct TpVector
{
	int32 vl_len_;					   /* varlena header (must be first) */
	int32 index_name_len;			   /* length of index name */
	int32 entry_count;				   /* number of term_id/frequency pairs */
	char  data[FLEXIBLE_ARRAY_MEMBER]; /* payload: index name + entries */
} TpVector;

/* Macros for accessing tpvector variable-length data */
#define TPVECTOR_INDEX_NAME_PTR(x) (((TpVector *)(x))->data)
#define TPVECTOR_ENTRIES_PTR(x)                  \
	((TpVectorEntry *)(((TpVector *)(x))->data + \
					   MAXALIGN(((TpVector *)(x))->index_name_len + 1)))

/* Function declarations */
Datum tpvector_in(PG_FUNCTION_ARGS);
Datum tpvector_out(PG_FUNCTION_ARGS);
Datum tpvector_recv(PG_FUNCTION_ARGS);
Datum tpvector_send(PG_FUNCTION_ARGS);

/* New constructor for string-based tpvector */
Datum to_tpvector(PG_FUNCTION_ARGS);

/* Operator functions */
Datum tpvector_score(PG_FUNCTION_ARGS);
Datum tpvector_eq(PG_FUNCTION_ARGS);

/* Convenience functions */
Datum tpvector_text_score(PG_FUNCTION_ARGS);
Datum tp_text_text_score(PG_FUNCTION_ARGS);
Datum text_tpvector_score(PG_FUNCTION_ARGS);

/* Utility functions */
TpVector *create_tpvector_from_strings(
		const char	*index_name,
		int			 entry_count,
		const char **lexemes,
		const int32 *frequencies);

char		  *get_tpvector_index_name(TpVector *tpvec);
TpVectorEntry *get_tpvector_first_entry(TpVector *vec);
TpVectorEntry *get_tpvector_next_entry(TpVectorEntry *current);
