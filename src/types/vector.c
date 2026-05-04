/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * vector.c - bm25vector data type implementation
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/relation.h>
#include <catalog/namespace.h>
#include <lib/stringinfo.h>
#include <libpq/pqformat.h>
#include <math.h>
#include <nodes/pg_list.h>
#include <nodes/value.h>
#include <stdlib.h>
#include <tsearch/ts_type.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/regproc.h>
#include <utils/rel.h>
#include <utils/syscache.h>
#include <varatt.h>

#include "access/am.h"
#include "constants.h"
#include "index/metapage.h"
#include "index/resolve.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "types/vector.h"

/* ---------------------------------------------------------------------
 * Legacy v1 detection
 *
 * pg_textsearch < 1.2.0 used a different on-disk bm25vector layout
 * (no magic, int32 freq + int32 lexeme_len per entry, MAXALIGN
 * padding). Storing bm25vector values in user tables was always
 * uncommon — the type is overwhelmingly used transiently for
 * query/insert paths — so we no longer support reading legacy v1
 * values. Detection of a non-v2 value produces a clear
 * REINDEX-or-recompute error instead of silently misinterpreting
 * the bytes.
 * ---------------------------------------------------------------------
 */

/* ---------------------------------------------------------------------
 * Varint helpers (LEB128 base-128 little-endian)
 * ---------------------------------------------------------------------
 */

size_t
tpvector_varint_encode(uint32 v, uint8 *out)
{
	size_t n = 0;

	while (v >= 0x80)
	{
		out[n++] = (uint8)(v | 0x80);
		v >>= 7;
	}
	out[n++] = (uint8)v;
	return n;
}

uint32
tpvector_varint_decode(const uint8 **cursor, const uint8 *end)
{
	uint32		 result = 0;
	int			 shift	= 0;
	const uint8 *p		= *cursor;

	while (p < end && (*p & 0x80) != 0)
	{
		result |= ((uint32)(*p & 0x7F)) << shift;
		shift += 7;
		p++;
		if (shift >= 32)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("bm25vector varint exceeds 32 bits")));
	}
	if (p >= end)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bm25vector varint truncated")));
	result |= ((uint32)*p) << shift;
	p++;
	*cursor = p;
	return result;
}

size_t
tpvector_varint_size(uint32 v)
{
	size_t n = 1;

	while (v >= 0x80)
	{
		v >>= 7;
		n++;
	}
	return n;
}

/* ---------------------------------------------------------------------
 * Local helpers
 * ---------------------------------------------------------------------
 */

typedef struct
{
	const char *lexeme;
	int32		frequency;
} LexemeFreqPair;

static int
strcmp_wrapper(const void *a, const void *b)
{
	const LexemeFreqPair *pair_a = (const LexemeFreqPair *)a;
	const LexemeFreqPair *pair_b = (const LexemeFreqPair *)b;

	return strcmp(pair_a->lexeme, pair_b->lexeme);
}

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

PG_FUNCTION_INFO_V1(tpvector_in);
PG_FUNCTION_INFO_V1(tpvector_out);
PG_FUNCTION_INFO_V1(tpvector_recv);
PG_FUNCTION_INFO_V1(tpvector_send);
PG_FUNCTION_INFO_V1(tpvector_eq);
PG_FUNCTION_INFO_V1(to_tpvector);

/* ---------------------------------------------------------------------
 * Format detection and v1 → v2 conversion
 * ---------------------------------------------------------------------
 */

bool
tpvector_is_v2(const void *raw)
{
	const char *bytes = (const char *)raw;

	if (raw == NULL || VARSIZE(raw) < (Size)(sizeof(int32) + 4))
		return false;

	bytes += sizeof(int32); /* skip vl_len_ */
	return bytes[0] == TPVECTOR_V2_MAGIC0 && bytes[1] == TPVECTOR_V2_MAGIC1 &&
		   bytes[2] == TPVECTOR_V2_MAGIC2 && bytes[3] == TPVECTOR_V2_MAGIC3;
}

/*
 * Validate a v2 TpVector's entry stream against the varlena's
 * declared length. Walks each entry, decoding varint freq + lex_len
 * and ensuring the lexeme bytes stay in bounds. Errors on any
 * inconsistency.
 *
 * Called from tpvector_canonicalize for every v2 input so that
 * operator entry points (tpvector_out, tpvector_eq, etc.) can
 * iterate entries without re-checking bounds at every read. The
 * pre-v2 code had per-entry checks scattered throughout each
 * operator; this centralizes them.
 */
static void
tpvector_validate_v2(TpVector *v)
{
	const uint8 *cursor;
	const uint8 *end;
	int			 i;

	if (VARSIZE(v) < sizeof(TpVector))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("v2 bm25vector too small: %u", VARSIZE(v))));

	if (v->version != TPVECTOR_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("unsupported bm25vector version: %u", v->version)));

	if (v->index_name_len < 0 || v->index_name_len > TP_MAX_INDEX_NAME_LENGTH)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid index name length in bm25vector: %d",
						v->index_name_len)));

	if (v->entry_count < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid entry count in bm25vector: %d",
						v->entry_count)));

	cursor = (const uint8 *)TPVECTOR_ENTRIES_PTR(v);
	end	   = (const uint8 *)v + VARSIZE(v);

	if ((const char *)cursor > (const char *)end)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("bm25vector header overruns varlena")));

	for (i = 0; i < v->entry_count; i++)
	{
		uint32 freq;
		uint32 lex_len;

		if (cursor >= end)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("v2 bm25vector entry %d truncated", i)));

		freq	= tpvector_varint_decode(&cursor, end);
		lex_len = tpvector_varint_decode(&cursor, end);
		(void)freq;

		if (cursor + lex_len > end)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("v2 bm25vector entry %d lexeme "
							"extends beyond buffer",
							i)));
		cursor += lex_len;
	}
}

TpVector *
tpvector_canonicalize(TpVector *raw)
{
	if (raw == NULL)
		return NULL;
	if (!tpvector_is_v2(raw))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bm25vector value uses unsupported legacy format"),
				 errdetail(
						 "Persisted bm25vector values from "
						 "pg_textsearch versions prior to 1.2.0 cannot "
						 "be read by this version."),
				 errhint("Recompute the value via "
						 "to_bm25vector(text, 'index_name') from the "
						 "source text, or DROP and recreate the column.")));
	tpvector_validate_v2(raw);
	return raw;
}

/* ---------------------------------------------------------------------
 * Entry iteration (v2)
 * ---------------------------------------------------------------------
 */

void
tpvector_entry_decode(const TpVectorEntry *entry, TpVectorEntryView *out)
{
	const uint8 *cursor = (const uint8 *)entry;
	const uint8 *end	= cursor + UINT32_MAX; /* effectively unbounded */

	out->frequency	= tpvector_varint_decode(&cursor, end);
	out->lexeme_len = tpvector_varint_decode(&cursor, end);
	out->lexeme		= (const char *)cursor;
}

TpVectorEntry *
get_tpvector_first_entry(TpVector *vec)
{
	if (!vec || vec->entry_count == 0)
		return NULL;
	return TPVECTOR_ENTRIES_PTR(vec);
}

TpVectorEntry *
get_tpvector_next_entry(TpVectorEntry *current)
{
	const uint8 *cursor;
	uint32		 freq;
	uint32		 lex_len;

	if (!current)
		return NULL;

	cursor	= (const uint8 *)current;
	freq	= tpvector_varint_decode(&cursor, cursor + UINT32_MAX);
	lex_len = tpvector_varint_decode(&cursor, cursor + UINT32_MAX);
	(void)freq;
	cursor += lex_len;
	return (TpVectorEntry *)cursor;
}

/* ---------------------------------------------------------------------
 * tpvector_in: text input
 * ---------------------------------------------------------------------
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

	colon_pos = strchr(str, ':');
	if (colon_pos == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type tpvector: \"%s\"", str),
				 errhint("Expected format: "
						 "\"index_name:{lexeme:freq,...}\"")));

	index_name_len = colon_pos - str;
	index_name	   = palloc(index_name_len + 1);
	memcpy(index_name, str, index_name_len);
	index_name[index_name_len] = '\0';

	entries_str = colon_pos + 1;

	if (!entries_str || strlen(entries_str) < 2 || entries_str[0] != '{' ||
		entries_str[strlen(entries_str) - 1] != '}')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid tpvector format: \"%s\"", str),
				 errhint("Entries must be enclosed in braces: "
						 "{lexeme:freq,...}")));

	entries_str++;
	end_ptr	 = entries_str + strlen(entries_str) - 1;
	*end_ptr = '\0';

	if (strlen(entries_str) > 0)
	{
		entry_count = 1;
		for (ptr = entries_str; *ptr; ptr++)
		{
			if (*ptr == ',')
				entry_count++;
		}
	}

	if (entry_count > 0)
	{
		lexemes		= palloc(entry_count * sizeof(char *));
		frequencies = palloc(entry_count * sizeof(int32));

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

			lexeme_len = entry_colon_pos - ptr;
			lexeme	   = palloc(lexeme_len + 1);
			memcpy(lexeme, ptr, lexeme_len);
			lexeme[lexeme_len] = '\0';
			lexemes[i]		   = lexeme;

			freq_str = entry_colon_pos + 1;
			if (comma_pos)
			{
				char saved = *comma_pos;

				*comma_pos	   = '\0';
				frequencies[i] = pg_strtoint32(freq_str);
				*comma_pos	   = saved;
				ptr			   = comma_pos + 1;
			}
			else
			{
				frequencies[i] = pg_strtoint32(freq_str);
			}

			/*
			 * v2 stores frequency as an unsigned varint; negative
			 * input is meaningless for BM25 and would silently
			 * clamp to zero. Reject explicitly so callers see a
			 * parse error rather than a silent normalization.
			 */
			if (frequencies[i] < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("negative frequency in tpvector entry: "
								"%d",
								frequencies[i])));
		}
	}

	result = create_tpvector_from_strings(
			index_name, entry_count, (const char **)lexemes, frequencies);

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

/* ---------------------------------------------------------------------
 * tpvector_out: text output (canonicalized)
 * ---------------------------------------------------------------------
 */

Datum
tpvector_out(PG_FUNCTION_ARGS)
{
	TpVector	  *tpvec;
	char		  *index_name = NULL;
	StringInfoData result;
	TpVectorEntry *entry;
	int			   i;

	tpvec = (TpVector *)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	if (!tpvec)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null tpvector passed to tpvector_out")));

	tpvec = tpvector_canonicalize(tpvec);

	if (VARSIZE(tpvec) < sizeof(TpVector))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid tpvector structure")));

	index_name = get_tpvector_index_name(tpvec);

	initStringInfo(&result);
	appendStringInfo(&result, "%s:{", index_name);

	entry = TPVECTOR_ENTRIES_PTR(tpvec);
	for (i = 0; i < tpvec->entry_count; i++)
	{
		TpVectorEntryView v;
		char			 *lexeme;
		bool			  use_heap;

		tpvector_entry_decode(entry, &v);

		if (i > 0)
			appendStringInfoChar(&result, ',');

		use_heap = v.lexeme_len >= 256;
		if (use_heap)
			lexeme = palloc(v.lexeme_len + 1);
		else
			lexeme = alloca(v.lexeme_len + 1);

		memcpy(lexeme, v.lexeme, v.lexeme_len);
		lexeme[v.lexeme_len] = '\0';

		appendStringInfo(&result, "%s:%u", lexeme, v.frequency);

		if (use_heap)
			pfree(lexeme);

		entry = get_tpvector_next_entry(entry);
	}

	appendStringInfoChar(&result, '}');

	pfree(index_name);

	PG_RETURN_CSTRING(result.data);
}

/* ---------------------------------------------------------------------
 * tpvector_recv: binary receive (accepts v1 + v2)
 * ---------------------------------------------------------------------
 */

Datum
tpvector_recv(PG_FUNCTION_ARGS)
{
	StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
	int		   total_size;
	TpVector  *result;

	total_size = pq_getmsgint(buf, 4);

	if (total_size < (int)sizeof(TpVector) || (Size)total_size > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid bm25vector binary size: %d", total_size)));

	result = (TpVector *)palloc(total_size);
	SET_VARSIZE(result, total_size);

	pq_copymsgbytes(
			buf, (char *)result + sizeof(int32), total_size - sizeof(int32));

	/*
	 * Defer to tpvector_canonicalize for magic-check + validation.
	 * Legacy v1 inputs (no magic, pre-1.2.0) are rejected with a
	 * clear error; v2 inputs have their entry stream bounds-checked
	 * via tpvector_validate_v2.
	 */
	PG_RETURN_POINTER(tpvector_canonicalize(result));
}

/* ---------------------------------------------------------------------
 * tpvector_send: binary send (always emits v2)
 * ---------------------------------------------------------------------
 */

Datum
tpvector_send(PG_FUNCTION_ARGS)
{
	TpVector	  *tpvec;
	StringInfoData buf;
	int			   total_size;

	tpvec = (TpVector *)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	tpvec = tpvector_canonicalize(tpvec);

	pq_begintypsend(&buf);

	total_size = VARSIZE(tpvec);
	pq_sendint32(&buf, total_size);
	pq_sendbytes(
			&buf, (char *)tpvec + sizeof(int32), total_size - sizeof(int32));

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* ---------------------------------------------------------------------
 * tpvector_eq: equality
 * ---------------------------------------------------------------------
 */

Datum
tpvector_eq(PG_FUNCTION_ARGS)
{
	TpVector *vec1 = (TpVector *)PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	TpVector *vec2 = (TpVector *)PG_DETOAST_DATUM(PG_GETARG_DATUM(1));
	char	 *index_name1;
	char	 *index_name2;
	bool	  result = true;
	int		  i;

	vec1 = tpvector_canonicalize(vec1);
	vec2 = tpvector_canonicalize(vec2);

	index_name1 = get_tpvector_index_name(vec1);
	index_name2 = get_tpvector_index_name(vec2);

	if (strcmp(index_name1, index_name2) != 0)
	{
		pfree(index_name1);
		pfree(index_name2);
		PG_RETURN_BOOL(false);
	}

	if (vec1->entry_count != vec2->entry_count)
	{
		pfree(index_name1);
		pfree(index_name2);
		PG_RETURN_BOOL(false);
	}

	{
		TpVectorEntry *e1 = get_tpvector_first_entry(vec1);

		for (i = 0; i < vec1->entry_count && result && e1; i++)
		{
			TpVectorEntryView v1;
			TpVectorEntry	 *e2		  = get_tpvector_first_entry(vec2);
			bool			  found_match = false;
			int				  j;

			tpvector_entry_decode(e1, &v1);

			for (j = 0; j < vec2->entry_count && e2; j++)
			{
				TpVectorEntryView v2;

				tpvector_entry_decode(e2, &v2);
				if (v1.lexeme_len == v2.lexeme_len &&
					v1.frequency == v2.frequency &&
					memcmp(v1.lexeme, v2.lexeme, v1.lexeme_len) == 0)
				{
					found_match = true;
					break;
				}
				e2 = get_tpvector_next_entry(e2);
			}

			if (!found_match)
				result = false;

			e1 = get_tpvector_next_entry(e1);
		}
	}

	pfree(index_name1);
	pfree(index_name2);
	PG_RETURN_BOOL(result);
}

/* ---------------------------------------------------------------------
 * Accessors
 * ---------------------------------------------------------------------
 */

char *
get_tpvector_index_name(TpVector *tpvec)
{
	int			 name_len;
	char		*index_name;
	char		*name_ptr;
	unsigned int vector_total_size;

	if (!tpvec)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null tpvector passed to get_tpvector_index_name")));

	vector_total_size = VARSIZE(tpvec);
	if (vector_total_size < sizeof(TpVector))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid tpvector size: %d", vector_total_size)));

	name_len = tpvec->index_name_len;

	if (name_len < 0 || name_len > TP_MAX_INDEX_NAME_LENGTH)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid index name length: %d", name_len)));

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

TpVector *
create_tpvector_from_strings(
		const char	*index_name,
		int			 entry_count,
		const char **lexemes,
		const int32 *frequencies)
{
	int				index_name_len;
	int				total_size;
	int				i;
	TpVector	   *result;
	LexemeFreqPair *pairs = NULL;

	if (!index_name)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("null index name to create_tpvector_from_strings")));

	index_name_len = strlen(index_name);
	if (index_name_len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty index name in create_tpvector_from_strings")));

	if (entry_count > 0 && lexemes && frequencies)
	{
		pairs = palloc(entry_count * sizeof(LexemeFreqPair));
		for (i = 0; i < entry_count; i++)
		{
			pairs[i].lexeme	   = lexemes[i];
			pairs[i].frequency = frequencies[i];
		}
		if (entry_count > 1)
			qsort(pairs, entry_count, sizeof(LexemeFreqPair), strcmp_wrapper);
	}

	total_size = MAXALIGN(sizeof(TpVector)) + MAXALIGN(index_name_len + 1);
	for (i = 0; i < entry_count; i++)
	{
		const char *lex	 = pairs ? pairs[i].lexeme
								 : (lexemes ? lexemes[i] : NULL);
		int32		freq = pairs ? pairs[i].frequency
								 : (frequencies ? frequencies[i] : 0);
		int			lex_len;

		if (!lex)
			continue;

		lex_len = strlen(lex);
		total_size += tpvector_varint_size((uint32)Max(freq, 0)) +
					  tpvector_varint_size((uint32)lex_len) + lex_len;
	}

	result = (TpVector *)palloc0(total_size);
	SET_VARSIZE(result, total_size);

	result->magic[0] = TPVECTOR_V2_MAGIC0;
	result->magic[1] = TPVECTOR_V2_MAGIC1;
	result->magic[2] = TPVECTOR_V2_MAGIC2;
	result->magic[3] = TPVECTOR_V2_MAGIC3;
	result->version	 = TPVECTOR_VERSION;

	result->index_name_len = index_name_len;
	result->entry_count	   = entry_count;

	memcpy(TPVECTOR_INDEX_NAME_PTR(result), index_name, index_name_len);
	*((char *)TPVECTOR_INDEX_NAME_PTR(result) + index_name_len) = '\0';

	if (pairs)
	{
		uint8 *entry_ptr = (uint8 *)TPVECTOR_ENTRIES_PTR(result);

		for (i = 0; i < entry_count; i++)
		{
			uint32 freq	   = (uint32)Max(pairs[i].frequency, 0);
			int	   lex_len = strlen(pairs[i].lexeme);

			entry_ptr += tpvector_varint_encode(freq, entry_ptr);
			entry_ptr += tpvector_varint_encode((uint32)lex_len, entry_ptr);
			memcpy(entry_ptr, pairs[i].lexeme, lex_len);
			entry_ptr += lex_len;
		}

		pfree(pairs);
	}

	return result;
}

/* ---------------------------------------------------------------------
 * to_tpvector: build a v2 vector from text + index name
 * ---------------------------------------------------------------------
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
	int				i;
	char		  **lexemes;
	int32		   *frequencies;
	int				entry_count;
	TpVector	   *result;

	index_name = text_to_cstring(index_name_text);

	index_oid = tp_resolve_index_name_shared(index_name);

	if (!OidIsValid(index_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("index \"%s\" not found", index_name)));

	index_rel = index_open(index_oid, AccessShareLock);
	if (!RelationIsValid(index_rel))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("could not open index \"%s\"", index_name)));

	metap = tp_get_metapage(index_rel);

	text_config_oid = metap->text_config_oid;
	if (!OidIsValid(text_config_oid))
		text_config_oid = DatumGetObjectId(DirectFunctionCall1(
				regconfigin, CStringGetDatum("english")));

	pfree(metap);
	index_close(index_rel, AccessShareLock);

	(void)tp_tokenize_text(
			input_text, text_config_oid,
			&lexemes, &frequencies, &entry_count);

	result = create_tpvector_from_strings(
			index_name, entry_count, (const char **)lexemes, frequencies);

	if (lexemes)
	{
		for (i = 0; i < entry_count; i++)
			pfree(lexemes[i]);
		pfree(lexemes);
	}
	if (frequencies)
		pfree(frequencies);
	pfree(index_name);

	PG_RETURN_POINTER(result);
}
