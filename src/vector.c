#include <postgres.h>

#include <access/genam.h>
#include <catalog/namespace.h>
#include <lib/stringinfo.h>
#include <libpq/pqformat.h>
#include <math.h>
#include <stdlib.h>
#include <tsearch/ts_type.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>
#include <utils/syscache.h>
#include <varatt.h>

#include "constants.h"
#include "index.h"
#include "memtable.h"
#include "metapage.h"
#include "posting.h"
#include "vector.h"

/* Helper structure for sorting lexemes */
typedef struct
{
	const char *lexeme;
	int32		frequency;
} LexemeFreqPair;

/* Helper function for qsort comparison of lexemes */
static int
strcmp_wrapper(const void *a, const void *b)
{
	const LexemeFreqPair *pair_a = (const LexemeFreqPair *)a;
	const LexemeFreqPair *pair_b = (const LexemeFreqPair *)b;

	return strcmp(pair_a->lexeme, pair_b->lexeme);
}

/* Helper macros and functions */
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

PG_FUNCTION_INFO_V1(tpvector_in);
PG_FUNCTION_INFO_V1(tpvector_out);
PG_FUNCTION_INFO_V1(tpvector_recv);
PG_FUNCTION_INFO_V1(tpvector_send);
PG_FUNCTION_INFO_V1(tpvector_eq);
PG_FUNCTION_INFO_V1(tp_score_texts);
PG_FUNCTION_INFO_V1(to_tpvector);

/*
 * tpvector input function
 * Format: "index_name:{lexeme1:freq1,lexeme2:freq2,...}"
 * Example: "my_index:{database:2,system:1,query:4}"
 */
Datum
tpvector_in(PG_FUNCTION_ARGS)
{
	char	 *str = PG_GETARG_CSTRING(0);
	char	 *colon_pos;
	char	 *index_name;
	char	 *entries_str;
	TpVector *result;
	int		  index_name_len;
	int		  entry_count = 0;
	char	**lexemes	  = NULL;
	int32	 *frequencies = NULL;
	char	 *ptr;
	char	 *end_ptr;
	int		  i = 0;

	/* Find the colon separator */
	colon_pos = strchr(str, ':');
	if (colon_pos == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type tpvector: \"%s\"", str),
				 errhint("Expected format: "
						 "\"index_name:{lexeme:freq,...}\"")));

	/* Extract index name */
	index_name_len = colon_pos - str;
	index_name	   = palloc(index_name_len + 1);
	memcpy(index_name, str, index_name_len);
	index_name[index_name_len] = '\0';

	/* Parse entries part */
	entries_str = colon_pos + 1;

	/* Validate entries_str starts with '{' and ends with '}' */
	if (!entries_str || strlen(entries_str) < 2 || entries_str[0] != '{' ||
		entries_str[strlen(entries_str) - 1] != '}')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid tpvector format: \"%s\"", str),
				 errhint("Entries must be enclosed in braces: "
						 "{lexeme:freq,...}")));

	/* Skip braces */
	entries_str++;
	end_ptr	 = entries_str + strlen(entries_str) - 1;
	*end_ptr = '\0';

	/* Count entries first */
	if (strlen(entries_str) > 0)
	{
		entry_count = 1;
		for (ptr = entries_str; *ptr; ptr++)
		{
			if (*ptr == ',')
				entry_count++;
		}
	}

	/* Allocate arrays for lexemes and frequencies */
	if (entry_count > 0)
	{
		lexemes		= palloc(entry_count * sizeof(char *));
		frequencies = palloc(entry_count * sizeof(int32));

		/* Parse each entry */
		ptr = entries_str;
		for (i = 0; i < entry_count; i++)
		{
			char *comma_pos		  = strchr(ptr, ',');
			char *entry_colon_pos = strchr(ptr, ':');
			char *lexeme;
			int	  lexeme_len;
			char *freq_str;

			if (!entry_colon_pos || (comma_pos && entry_colon_pos > comma_pos))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid entry format in tpvector: \"%s\"",
								ptr)));

			/* Extract lexeme */
			lexeme_len = entry_colon_pos - ptr;
			lexeme	   = palloc(lexeme_len + 1);
			memcpy(lexeme, ptr, lexeme_len);
			lexeme[lexeme_len] = '\0';
			lexemes[i]		   = lexeme;

			/* Extract frequency */
			freq_str = entry_colon_pos + 1;
			if (comma_pos)
			{
				char saved = *comma_pos;

				*comma_pos	   = '\0';
				frequencies[i] = atoi(freq_str);
				*comma_pos	   = saved;
				ptr			   = comma_pos + 1;
			}
			else
			{
				frequencies[i] = atoi(freq_str);
			}
		}
	}

	/* Create tpvector */
	result = create_tpvector_from_strings(
			index_name, entry_count, (const char **)lexemes, frequencies);

	/* Cleanup */
	pfree(index_name);
	if (lexemes)
	{
		for (i = 0; i < entry_count; i++)
			pfree(lexemes[i]);
		pfree(lexemes);
	}
	if (frequencies)
		pfree(frequencies);

	PG_RETURN_POINTER(result);
}

/*
 * tpvector output function
 * Outputs in format: "index_name:{lexeme1:freq1,lexeme2:freq2,...}"
 */
Datum
tpvector_out(PG_FUNCTION_ARGS)
{
	TpVector	  *tpvec;
	char		  *index_name = NULL;
	StringInfoData result;
	TpVectorEntry *entry;
	int			   i;

	tpvec = (TpVector *)PG_GETARG_POINTER(0);

	/* Basic validation */
	if (!tpvec)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null tpvector passed to tpvector_out")));

	/* Detoast the input if necessary */
	tpvec = (TpVector *)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));

	/* Additional validation after detoasting */
	if (!tpvec || VARSIZE(tpvec) < sizeof(TpVector))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid tpvector structure")));

	/* Get index name component */
	index_name = get_tpvector_index_name(tpvec);
	if (!index_name)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("failed to extract index name from tpvector")));

	/* Build output string */
	initStringInfo(&result);
	appendStringInfo(&result, "%s:{", index_name);

	/* Add each entry - use stored strings directly */
	entry = TPVECTOR_ENTRIES_PTR(tpvec);
	for (i = 0; i < tpvec->entry_count; i++)
	{
		char *lexeme;
		bool  use_heap = false;

		if (i > 0)
			appendStringInfoChar(&result, ',');

		if (entry->lexeme_len < 1024)
		{
			lexeme = alloca(entry->lexeme_len + 1);
		}
		else
		{
			lexeme	 = palloc(entry->lexeme_len + 1);
			use_heap = true;
		}

		memcpy(lexeme, entry->lexeme, entry->lexeme_len);
		lexeme[entry->lexeme_len] = '\0';

		appendStringInfo(&result, "%s:%d", lexeme, entry->frequency);

		/* Free only if we used heap allocation */
		if (use_heap)
			pfree(lexeme);

		entry = get_tpvector_next_entry(entry);
	}

	appendStringInfoChar(&result, '}');

	/* Clean up allocated memory */
	pfree(index_name);

	PG_RETURN_CSTRING(result.data);
}

/*
 * tpvector binary receive function
 */
Datum
tpvector_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
	int		   total_size;
	TpVector  *result;

	/* Read total size */
	total_size = pq_getmsgint(buf, 4);

	/* Allocate and read the entire structure */
	result = (TpVector *)palloc(total_size);
	SET_VARSIZE(result, total_size);

	/* Read the rest of the structure after varlena header */
	pq_copymsgbytes(
			buf, (char *)result + sizeof(int32), total_size - sizeof(int32));

	PG_RETURN_POINTER(result);
}

/*
 * tpvector binary send function
 */
Datum
tpvector_send(PG_FUNCTION_ARGS)
{
	TpVector	  *tpvec = (TpVector *)PG_GETARG_POINTER(0);
	StringInfoData buf;
	int			   total_size;

	pq_begintypsend(&buf);

	/* Send total size */
	total_size = VARSIZE(tpvec);
	pq_sendint32(&buf, total_size);

	/* Send the entire structure after varlena header */
	pq_sendbytes(
			&buf, (char *)tpvec + sizeof(int32), total_size - sizeof(int32));

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Validate tpvector inputs and extract index name
 */
static char *
validate_tpvector_inputs(TpVector *doc_vec, TpVector *query_vec)
{
	char *doc_index_name;
	char *query_index_name;
	char *index_name;

	if (!doc_vec || !query_vec)
		return NULL;

	if (VARSIZE(doc_vec) < sizeof(TpVector) ||
		VARSIZE(query_vec) < sizeof(TpVector))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid tpvector structure size")));
	}

	/* Both vectors should use the same index */
	doc_index_name	 = get_tpvector_index_name(doc_vec);
	query_index_name = get_tpvector_index_name(query_vec);

	if (strcmp(doc_index_name, query_index_name) != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tpvector operands must use the same index"),
				 errhint("Document vector uses index \"%s\", query vector "
						 "uses index \"%s\"",
						 doc_index_name,
						 query_index_name)));
	}

	index_name = pstrdup(doc_index_name);
	pfree(doc_index_name);
	pfree(query_index_name);

	return index_name;
}

/*
 * Setup index relation and get BM25 parameters and corpus statistics
 */
static bool
setup_bm25_context(
		const char		*index_name,
		Oid				*index_oid_out,
		Relation		*index_rel_out,
		TpIndexMetaPage *metap_out,
		TpIndexState   **index_state_out,
		float4			*k1_out,
		float4			*b_out,
		int32			*total_docs_out,
		float4			*avg_doc_len_out)
{
	Oid				index_oid;
	Relation		index_rel;
	TpIndexMetaPage metap;
	TpIndexState   *index_state;

	/* Look up the index in current search path */
	index_oid = RelnameGetRelid(index_name);
	if (!OidIsValid(index_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" does not exist", index_name)));

	/* Open the index relation */
	index_rel = index_open(index_oid, AccessShareLock);

	/* Get index metadata */
	metap = tp_get_metapage(index_rel);
	if (!metap)
	{
		index_close(index_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not read BM25 index metadata")));
	}

	/* Get the index state from shared memory */
	index_state = tp_get_index_state(index_oid);
	if (!index_state)
	{
		pfree(metap);
		index_close(index_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("could not access BM25 index state")));
	}

	/* Calculate average IDF if not already done (lazy calculation) */
	if (index_state->stats.average_idf <= 0.0001f &&
		index_state->stats.total_docs > 0)
	{
		elog(NOTICE,
			 "Triggering lazy average IDF calculation (current: %.6f)",
			 index_state->stats.average_idf);
		tp_calculate_average_idf(index_state);
		elog(NOTICE,
			 "Lazy calculated average IDF: %.6f",
			 index_state->stats.average_idf);
	}

	/* Set output parameters */
	*index_oid_out	 = index_oid;
	*index_rel_out	 = index_rel;
	*metap_out		 = metap;
	*index_state_out = index_state;
	*k1_out			 = metap->k1;
	*b_out			 = metap->b;
	*total_docs_out	 = index_state->stats.total_docs;
	*avg_doc_len_out = index_state->stats.total_docs > 0
							 ? (float4)(index_state->stats.total_len /
										(double)index_state->stats.total_docs)
							 : 0.0f;

	return true;
}

/*
 * Calculate document length from tpvector
 */
static float4
calculate_doc_length(TpVector *doc_vec)
{
	int			   doc_count  = doc_vec->entry_count;
	TpVectorEntry *doc_entry  = get_tpvector_first_entry(doc_vec);
	float4		   doc_length = 0.0;
	int			   doc_idx;

	for (doc_idx = 0; doc_idx < doc_count && doc_entry; doc_idx++)
	{
		doc_length += doc_entry->frequency;
		if (doc_idx < doc_count - 1)
			doc_entry = get_tpvector_next_entry(doc_entry);
	}

	return doc_length;
}

/*
 * Find term frequency in document vector for a specific query term
 */
static float4
find_term_frequency(TpVector *doc_vec, TpVectorEntry *query_entry)
{
	int			   doc_count = doc_vec->entry_count;
	TpVectorEntry *doc_entry = get_tpvector_first_entry(doc_vec);
	int			   doc_idx;

	for (doc_idx = 0; doc_idx < doc_count && doc_entry; doc_idx++)
	{
		if (query_entry->lexeme_len == doc_entry->lexeme_len &&
			memcmp(query_entry->lexeme,
				   doc_entry->lexeme,
				   query_entry->lexeme_len) == 0)
			return doc_entry->frequency;

		if (doc_idx < doc_count - 1)
			doc_entry = get_tpvector_next_entry(doc_entry);
	}

	return 0.0;
}

/*
 * Calculate BM25 term score
 */
static float4
calculate_bm25_term_score(
		float4 tf,
		float4 idf,
		int	   query_freq,
		float4 k1,
		float4 b,
		float4 doc_length,
		float4 avg_doc_len)
{
	double numerator_d = (double)tf * ((double)k1 + 1.0);
	double denominator_d;
	float4 term_score;

	/* Avoid division by zero - if avg_doc_len is 0, use simplified formula */
	if (avg_doc_len > 0.0f)
	{
		denominator_d = (double)tf +
						(double)k1 * (1.0 - (double)b +
									  (double)b * ((double)doc_length /
												   (double)avg_doc_len));
	}
	else
	{
		/* When avg_doc_len is 0 (no corpus stats), fall back to standard TF */
		denominator_d = (double)tf + (double)k1;
	}

	term_score = (float4)((double)idf * (numerator_d / denominator_d) *
						  (double)query_freq);

	/* Debug NaN detection */
	if (isnan(term_score))
		elog(WARNING,
			 "NaN detected in BM25 term score calculation: "
			 "idf=%f, numerator_d=%f, denominator_d=%f, query_freq=%d, "
			 "tf=%f, doc_len=%f, avg_doc_len=%f, k1=%f, b=%f",
			 idf,
			 numerator_d,
			 denominator_d,
			 query_freq,
			 tf,
			 doc_length,
			 avg_doc_len,
			 k1,
			 b);

	return term_score;
}

/*
 * Equality function: tpvector = tpvector → boolean
 */
Datum
tpvector_eq(PG_FUNCTION_ARGS)
{
	TpVector *vec1 = (TpVector *)PG_GETARG_POINTER(0);
	TpVector *vec2 = (TpVector *)PG_GETARG_POINTER(1);
	char	 *index_name1;
	char	 *index_name2;
	bool	  result = true;
	int		  i;

	/* Compare index names first */
	index_name1 = get_tpvector_index_name(vec1);
	index_name2 = get_tpvector_index_name(vec2);

	if (strcmp(index_name1, index_name2) != 0)
	{
		pfree(index_name1);
		pfree(index_name2);
		PG_RETURN_BOOL(false);
	}

	/* Compare entry counts */
	if (vec1->entry_count != vec2->entry_count)
	{
		pfree(index_name1);
		pfree(index_name2);
		PG_RETURN_BOOL(false);
	}

	/*
	 * For proper equality comparison, we need to check that both vectors
	 * contain the same terms with the same frequencies, regardless of order.
	 * We'll do this by checking each term in vec1 exists in vec2.
	 */

	/* Compare entries - we need to iterate through variable-length entries */
	{
		TpVectorEntry *entry1 = get_tpvector_first_entry(vec1);

		for (i = 0; i < vec1->entry_count && result && entry1; i++)
		{
			bool		   found_match = false;
			TpVectorEntry *entry2	   = get_tpvector_first_entry(vec2);
			int			   j;

			/* Look for this term in vec2 */
			for (j = 0; j < vec2->entry_count && entry2; j++)
			{
				if (entry1->lexeme_len == entry2->lexeme_len &&
					entry1->frequency == entry2->frequency &&
					memcmp(entry1->lexeme,
						   entry2->lexeme,
						   entry1->lexeme_len) == 0)
				{
					found_match = true;
					break;
				}
				entry2 = get_tpvector_next_entry(entry2);
			}

			if (!found_match)
				result = false;

			entry1 = get_tpvector_next_entry(entry1);
		}
	}

	pfree(index_name1);
	pfree(index_name2);
	PG_RETURN_BOOL(result);
}

/*
 * Utility function to extract index name from tpvector
 */
char *
get_tpvector_index_name(TpVector *tpvec)
{
	int	  name_len;
	char *index_name;
	char *name_ptr;
	int	  vector_total_size;

	/* Validate input */
	if (!tpvec)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null tpvector passed to get_tpvector_index_name")));

	/* Validate vector structure size */
	vector_total_size = VARSIZE(tpvec);
	if (vector_total_size < sizeof(TpVector))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid tpvector size: %d", vector_total_size)));

	name_len = tpvec->index_name_len;

	/* Validate name length */
	if (name_len < 0 || name_len > TP_MAX_INDEX_NAME_LENGTH)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid index name length: %d", name_len)));

	/* Validate that name pointer is within vector bounds */
	name_ptr = TPVECTOR_INDEX_NAME_PTR(tpvec);
	if (name_ptr < (char *)tpvec ||
		name_ptr + name_len > (char *)tpvec + vector_total_size)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("index name data extends beyond vector bounds")));

	index_name = palloc(name_len + 1);
	memcpy(index_name, name_ptr, name_len);
	index_name[name_len] = '\0';

	return index_name;
}

/*
 * Utility function to create a tpvector from string lexemes
 */
TpVector *
create_tpvector_from_strings(
		const char	*index_name,
		int			 entry_count,
		const char **lexemes,
		const int32 *frequencies)
{
	int		  index_name_len;
	int		  total_size;
	int		  i;
	TpVector *result;

	/* Validate inputs */
	if (!index_name)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null index name to create_tpvector_from_strings")));

	index_name_len = strlen(index_name);
	if (index_name_len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty index name in create_tpvector_from_strings")));

	/* Calculate total size needed - entries are variable length */
	total_size = MAXALIGN(sizeof(TpVector)) + MAXALIGN(index_name_len + 1);

	/* Add space for each variable-length entry */
	for (i = 0; i < entry_count; i++)
	{
		if (lexemes && lexemes[i])
		{
			int lexeme_len = strlen(lexemes[i]);

			total_size += MAXALIGN(sizeof(TpVectorEntry) + lexeme_len);
		}
	}

	/* Allocate and initialize */
	result = (TpVector *)palloc0(total_size);
	SET_VARSIZE(result, total_size);
	result->index_name_len = index_name_len;
	result->entry_count	   = entry_count;

	/* Copy index name */
	memcpy(TPVECTOR_INDEX_NAME_PTR(result), index_name, index_name_len);
	*((char *)TPVECTOR_INDEX_NAME_PTR(result) + index_name_len) = '\0';

	/* Sort entries by lexeme for consistent ordering */
	if (lexemes && frequencies && entry_count > 0)
	{
		/* Create temporary array for sorting */
		LexemeFreqPair *pairs = palloc(entry_count * sizeof(LexemeFreqPair));
		char		   *entry_ptr;

		/* Copy to temporary array */
		for (i = 0; i < entry_count; i++)
		{
			pairs[i].lexeme	   = lexemes[i];
			pairs[i].frequency = frequencies[i];
		}

		/* Sort by lexeme string */
		if (entry_count > 1)
		{
			qsort(pairs, entry_count, sizeof(LexemeFreqPair), strcmp_wrapper);
		}

		/* Store sorted entries */
		entry_ptr = (char *)TPVECTOR_ENTRIES_PTR(result);
		for (i = 0; i < entry_count; i++)
		{
			TpVectorEntry *entry	  = (TpVectorEntry *)entry_ptr;
			int			   lexeme_len = strlen(pairs[i].lexeme);

			entry->frequency  = pairs[i].frequency;
			entry->lexeme_len = lexeme_len;
			memcpy(entry->lexeme, pairs[i].lexeme, lexeme_len);
			/* No null terminator needed - we store the length */

			/* Move to next entry position */
			entry_ptr += MAXALIGN(sizeof(TpVectorEntry) + lexeme_len);
		}

		pfree(pairs);
	}

	return result;
}

/*
 * Helper function to get first entry from tpvector
 */
TpVectorEntry *
get_tpvector_first_entry(TpVector *vec)
{
	if (!vec || vec->entry_count == 0)
		return NULL;
	return TPVECTOR_ENTRIES_PTR(vec);
}

/*
 * Helper function to iterate through tpvector entries (variable length)
 */
TpVectorEntry *
get_tpvector_next_entry(TpVectorEntry *current)
{
	if (!current)
		return NULL;

	/* Move to next entry - entries are variable length */
	return (TpVectorEntry *)((char *)current + MAXALIGN(
													   sizeof(TpVectorEntry) +
													   current->lexeme_len));
}

/*
 * to_tpvector(text, index_name) - Create a tpvector from text using index's
 * config
 */
Datum
to_tpvector(PG_FUNCTION_ARGS)
{
	text		   *input_text		= PG_GETARG_TEXT_PP(0);
	text		   *index_name_text = PG_GETARG_TEXT_PP(1);
	char		   *index_name;
	Oid				index_oid;
	Oid				text_config_oid;
	Relation		index_rel = NULL;
	TpIndexMetaPage metap	  = NULL;
	text		   *config_text;
	Datum			tsvector_datum;
	TSVector		tsvector;
	WordEntry	   *we;
	int				i;
	char		  **lexemes;
	int32		   *frequencies;
	int				entry_count;
	TpVector	   *result;
	List		   *search_path;
	ListCell	   *lc;

	/* Extract index name */
	index_name = text_to_cstring(index_name_text);

	/* Look up index OID using direct catalog access */
	/* Try current search path */
	search_path = fetch_search_path(false);

	foreach (lc, search_path)
	{
		Oid namespace_oid = lfirst_oid(lc);

		index_oid = GetSysCacheOid2(
				RELNAMENSP,
				Anum_pg_class_oid,
				PointerGetDatum(index_name),
				ObjectIdGetDatum(namespace_oid));
		if (OidIsValid(index_oid))
			break;
	}

	list_free(search_path);

	if (!OidIsValid(index_oid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("index \"%s\" does not exist", index_name)));
	}

	/* Open the index relation to get metadata */
	index_rel = RelationIdGetRelation(index_oid);
	if (!RelationIsValid(index_rel))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open index \"%s\"", index_name)));
	}

	/* Get the metapage to extract text_config_oid */
	metap = tp_get_metapage(index_rel);

	/* Get the text config OID and name */
	text_config_oid = metap->text_config_oid;

	if (OidIsValid(text_config_oid))
	{
		/* Use the stored text config OID - convert to regconfig text */
		char *config_cstr = DatumGetCString(DirectFunctionCall1(
				regconfigout, ObjectIdGetDatum(text_config_oid)));

		config_text = cstring_to_text(config_cstr);
		pfree(config_cstr);
	}
	else
	{
		/* No configuration found, use default */
		config_text		= cstring_to_text("english");
		text_config_oid = InvalidOid;
	}

	/* Clean up index relation and metapage */
	pfree(metap);
	RelationClose(index_rel);

	/* Use to_tsvector to process the text */

	/*
	 * Call to_tsvector with config OID - use to_tsvector_byid for direct OID
	 * call
	 */
	if (OidIsValid(text_config_oid))
	{
		/* Use the OID directly */
		tsvector_datum = DirectFunctionCall2(
				to_tsvector_byid,
				ObjectIdGetDatum(text_config_oid),
				PointerGetDatum(input_text));
	}
	else
	{
		/* Fallback to using text config name */
		tsvector_datum = DirectFunctionCall2(
				to_tsvector,
				PointerGetDatum(config_text),
				PointerGetDatum(input_text));
	}
	tsvector = DatumGetTSVector(tsvector_datum);

	/* Count entries and allocate arrays */
	entry_count = tsvector->size;
	if (entry_count > 0)
	{
		lexemes		= palloc(entry_count * sizeof(char *));
		frequencies = palloc(entry_count * sizeof(int32));

		/* Extract lexemes and frequencies from tsvector */
		we = ARRPTR(tsvector);
		for (i = 0; i < entry_count; i++)
		{
			char *lexeme_start = STRPTR(tsvector) + we[i].pos;
			int	  lexeme_len   = we[i].len;

			lexemes[i] = palloc(lexeme_len + 1);
			memcpy(lexemes[i], lexeme_start, lexeme_len);
			lexemes[i][lexeme_len] = '\0';

			/* Count positions as frequency (or 1 if no positions) */
			if (we[i].haspos)
				frequencies[i] = POSDATALEN(tsvector, &we[i]);
			else
				frequencies[i] = 1;
		}
	}
	else
	{
		lexemes		= NULL;
		frequencies = NULL;
	}

	/* Create the tpvector */
	result = create_tpvector_from_strings(
			index_name, entry_count, (const char **)lexemes, frequencies);

	/* Cleanup */
	if (lexemes)
	{
		for (i = 0; i < entry_count; i++)
			pfree(lexemes[i]);
		pfree(lexemes);
	}
	if (frequencies)
		pfree(frequencies);
	pfree(index_name);
	/* Don't free StringInfo data - it's managed by memory context */

	PG_RETURN_POINTER(result);
}
