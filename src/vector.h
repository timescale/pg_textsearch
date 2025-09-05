#ifndef VECTOR_H
#define VECTOR_H

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "memtable.h"

/*
 * Term frequency entry for tpvector format
 * Stores variable-length lexemes with their frequencies
 */
typedef struct TpVectorEntry
{
	int32		frequency;		/* Term frequency in document */
	int32		lexeme_len;		/* Length of lexeme string */
	char		lexeme[FLEXIBLE_ARRAY_MEMBER];	/* Variable-length lexeme
												 * string */
}			TpVectorEntry;

/* tpvector data type structure */
typedef struct
{
	int32		vl_len_;		/* varlena header (must be first) */
	int32		index_name_len; /* length of index name */
	int32		entry_count;	/* number of term_id/frequency pairs */
	/* index name data follows immediately after */
	/* array of TpVectorEntry follows after index name */
}			TpVector;

/* Macros for accessing tpvector components */
#define TPVECTOR_INDEX_NAME_LEN(x) (((TpVector *) (x))->index_name_len)
#define TPVECTOR_ENTRY_COUNT(x) (((TpVector *) (x))->entry_count)
#define TPVECTOR_INDEX_NAME_PTR(x)                                           \
	((char *) (x) + MAXALIGN(sizeof(TpVector)))
#define TPVECTOR_ENTRIES_PTR(x)                                              \
	((TpVectorEntry *) (TPVECTOR_INDEX_NAME_PTR(x) +                       \
						  MAXALIGN(TPVECTOR_INDEX_NAME_LEN(x) + 1)))

/* Function declarations */
Datum		tpvector_in(PG_FUNCTION_ARGS);
Datum		tpvector_out(PG_FUNCTION_ARGS);
Datum		tpvector_recv(PG_FUNCTION_ARGS);
Datum		tpvector_send(PG_FUNCTION_ARGS);

/* New constructor for string-based tpvector */
Datum		to_tpvector(PG_FUNCTION_ARGS);

/* Operator functions */
Datum		tpvector_score(PG_FUNCTION_ARGS);
Datum		tpvector_eq(PG_FUNCTION_ARGS);

/* Convenience functions */
Datum		text_tpvector_score(PG_FUNCTION_ARGS);

/* Utility functions */
TpVector *create_tpvector_from_strings(const char *index_name,
										   int entry_count,
										   const char **lexemes,
										   const int32 *frequencies);
char	   *get_tpvector_index_name(TpVector * bm25vec);
TpVectorEntry *get_tpvector_first_entry(TpVector * vec);
TpVectorEntry *get_tpvector_next_entry(TpVectorEntry * current);

#endif							/* VECTOR_H */
