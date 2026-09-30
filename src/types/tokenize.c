/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * The parser/dictionary pipeline below is adapted from PostgreSQL's
 * src/backend/tsearch/ts_parse.c, Copyright (c) 1996-2025 PostgreSQL
 * Global Development Group, and is used under the PostgreSQL License.
 */
#include <postgres.h>

#include <common/hashfn.h>
#include <mb/pg_wchar.h>
#include <tsearch/ts_cache.h>
#include <tsearch/ts_utils.h>
#include <utils/fmgrprotos.h>
#include <utils/hsearch.h>
#include <varatt.h>

#include "types/tokenize.h"

typedef struct TpParsedLex
{
	int					type;
	char			   *lemm;
	int					lenlemm;
	int					window_id;
	struct TpParsedLex *next;
} TpParsedLex;

typedef struct TpParsedLexList
{
	TpParsedLex *head;
	TpParsedLex *tail;
} TpParsedLexList;

typedef struct TpLexizeData
{
	TSConfigCacheEntry *cfg;
	Oid					cur_dict_id;
	int					dict_position;
	DictSubState		dict_state;
	TpParsedLex		   *current_sublexeme;
	TpParsedLexList		work;
	TpParsedLexList		waste;
	TpParsedLex		   *last_result;
	TSLexeme		   *temporary_result;
	int					origin_window;
} TpLexizeData;

typedef void (*TpLexemeSink)(
		void  *context,
		char  *lexeme,
		int	   length,
		int	   global_position,
		int	   window_id,
		int	   window_position,
		uint16 flags,
		uint16 variant);

typedef struct TpParsedTextSink
{
	ParsedText *parsed;
} TpParsedTextSink;

typedef struct TpTermFrequencyEntry
{
	char *term;
	int32 frequency;
	int32 window_frequency;
	int	  last_window;
	int	  last_position;
} TpTermFrequencyEntry;

typedef struct TpTermFrequencySink
{
	HTAB *terms;
} TpTermFrequencySink;

static void
tp_lexize_init(TpLexizeData *state, TSConfigCacheEntry *cfg)
{
	state->cfg				 = cfg;
	state->cur_dict_id		 = InvalidOid;
	state->dict_position	 = 0;
	state->current_sublexeme = NULL;
	state->work.head		 = NULL;
	state->work.tail		 = NULL;
	state->waste.head		 = NULL;
	state->waste.tail		 = NULL;
	state->last_result		 = NULL;
	state->temporary_result	 = NULL;
	state->origin_window	 = 0;
}

static void
tp_parsed_lex_list_append(TpParsedLexList *list, TpParsedLex *lexeme)
{
	if (list->tail != NULL)
	{
		list->tail->next = lexeme;
		list->tail		 = lexeme;
	}
	else
		list->head = list->tail = lexeme;

	lexeme->next = NULL;
}

static TpParsedLex *
tp_parsed_lex_list_remove_head(TpParsedLexList *list)
{
	TpParsedLex *result = list->head;

	if (list->head != NULL)
		list->head = list->head->next;
	if (list->head == NULL)
		list->tail = NULL;

	return result;
}

static void
tp_lexize_add(
		TpLexizeData *state, int type, char *lexeme, int length, int window_id)
{
	TpParsedLex *parsed = palloc(sizeof(*parsed));

	parsed->type	  = type;
	parsed->lemm	  = lexeme;
	parsed->lenlemm	  = length;
	parsed->window_id = window_id;
	tp_parsed_lex_list_append(&state->work, parsed);
	state->current_sublexeme = state->work.tail;
}

static void
tp_parsed_lex_list_free(TpParsedLex *lexeme)
{
	while (lexeme != NULL)
	{
		TpParsedLex *next = lexeme->next;

		pfree(lexeme);
		lexeme = next;
	}
}

static void
tp_lexize_remove_head(TpLexizeData *state)
{
	tp_parsed_lex_list_append(
			&state->waste, tp_parsed_lex_list_remove_head(&state->work));
	state->dict_position = 0;
}

static void
tp_lexize_set_corresponding(TpLexizeData *state, TpParsedLex **corresponding)
{
	if (corresponding != NULL)
		*corresponding = state->waste.head;
	else
	{
		TpParsedLex *current = state->waste.head;

		while (current != NULL)
		{
			TpParsedLex *next = current->next;

			pfree(current);
			current = next;
		}
	}

	state->waste.head = state->waste.tail = NULL;
}

static void
tp_lexize_move_to_waste(TpLexizeData *state, TpParsedLex *stop)
{
	bool keep_going = true;

	while (state->work.head != NULL && keep_going)
	{
		if (state->work.head == stop)
		{
			state->current_sublexeme = stop->next;
			keep_going				 = false;
		}
		tp_lexize_remove_head(state);
	}
}

static void
tp_lexize_set_temporary_result(
		TpLexizeData *state, TpParsedLex *lexeme, TSLexeme *result)
{
	if (state->temporary_result != NULL)
	{
		TSLexeme *current;

		for (current = state->temporary_result; current->lexeme; current++)
			pfree(current->lexeme);
		pfree(state->temporary_result);
	}

	state->temporary_result = result;
	state->last_result		= lexeme;
}

static TSLexeme *
tp_lexize_exec(
		TpLexizeData *state, TpParsedLex **corresponding, int *result_window)
{
	ListDictionary		   *map;
	TSDictionaryCacheEntry *dictionary;
	TSLexeme			   *result;
	int						i;

	if (state->cur_dict_id == InvalidOid)
	{
		while (state->work.head != NULL)
		{
			TpParsedLex *current = state->work.head;
			char		*lexeme	 = current->lemm;
			int			 length	 = current->lenlemm;

			state->origin_window = current->window_id;
			map					 = state->cfg->map + current->type;
			if (current->type == 0 || current->type >= state->cfg->lenmap ||
				map->len == 0)
			{
				tp_lexize_remove_head(state);
				continue;
			}

			for (i = state->dict_position; i < map->len; i++)
			{
				dictionary = lookup_ts_dictionary_cache(map->dictIds[i]);
				state->dict_state.isend			= false;
				state->dict_state.getnext		= false;
				state->dict_state.private_state = NULL;
				result = (TSLexeme *)DatumGetPointer(FunctionCall4(
						&dictionary->lexize,
						PointerGetDatum(dictionary->dictData),
						PointerGetDatum(lexeme),
						Int32GetDatum(length),
						PointerGetDatum(&state->dict_state)));

				if (state->dict_state.getnext)
				{
					state->cur_dict_id	 = DatumGetObjectId(map->dictIds[i]);
					state->dict_position = i + 1;
					state->current_sublexeme = current->next;
					if (result != NULL)
						tp_lexize_set_temporary_result(state, current, result);
					return tp_lexize_exec(state, corresponding, result_window);
				}

				if (result == NULL)
					continue;

				if (result->flags & TSL_FILTER)
				{
					lexeme = result->lexeme;
					length = strlen(result->lexeme);
					continue;
				}

				tp_lexize_remove_head(state);
				tp_lexize_set_corresponding(state, corresponding);
				if (result_window != NULL)
					*result_window = state->origin_window;
				return result;
			}

			tp_lexize_remove_head(state);
		}
	}
	else
	{
		dictionary = lookup_ts_dictionary_cache(state->cur_dict_id);

		while (state->current_sublexeme != NULL)
		{
			TpParsedLex *current = state->current_sublexeme;

			map = state->cfg->map + current->type;
			if (current->type != 0)
			{
				bool dictionary_exists = false;

				if (current->type >= state->cfg->lenmap || map->len == 0)
				{
					state->current_sublexeme = current->next;
					continue;
				}

				for (i = 0; i < map->len && !dictionary_exists; i++)
					dictionary_exists = state->cur_dict_id ==
										DatumGetObjectId(map->dictIds[i]);

				if (!dictionary_exists)
				{
					state->cur_dict_id = InvalidOid;
					return tp_lexize_exec(state, corresponding, result_window);
				}
			}

			state->dict_state.isend	  = current->type == 0;
			state->dict_state.getnext = false;
			result = (TSLexeme *)DatumGetPointer(FunctionCall4(
					&dictionary->lexize,
					PointerGetDatum(dictionary->dictData),
					PointerGetDatum(current->lemm),
					Int32GetDatum(current->lenlemm),
					PointerGetDatum(&state->dict_state)));

			if (state->dict_state.getnext)
			{
				state->current_sublexeme = current->next;
				if (result != NULL)
					tp_lexize_set_temporary_result(state, current, result);
				continue;
			}

			if (result != NULL || state->temporary_result != NULL)
			{
				if (result != NULL)
					tp_lexize_move_to_waste(state, state->current_sublexeme);
				else
				{
					result = state->temporary_result;
					tp_lexize_move_to_waste(state, state->last_result);
				}

				state->cur_dict_id		= InvalidOid;
				state->dict_position	= 0;
				state->last_result		= NULL;
				state->temporary_result = NULL;
				tp_lexize_set_corresponding(state, corresponding);
				if (result_window != NULL)
					*result_window = state->origin_window;
				return result;
			}

			state->cur_dict_id = InvalidOid;
			return tp_lexize_exec(state, corresponding, result_window);
		}
	}

	tp_lexize_set_corresponding(state, corresponding);
	return NULL;
}

static int
tp_clip_token_length(const char *token, int length, int max_token_length)
{
	if (max_token_length == 0 || length <= max_token_length)
		return length;

	return pg_mbcliplen(token, length, max_token_length);
}

int
tp_token_window_end(const char *input, int input_length, int window_start)
{
	const char *data;
	int			remaining;
	int			target;
	int			i;
	int			pos;

	Assert(window_start >= 0);
	Assert(window_start < input_length);

	remaining = input_length - window_start;
	if (remaining <= TP_TOKEN_WINDOW_BYTES)
		return input_length;

	data   = input + window_start;
	target = TP_TOKEN_WINDOW_BYTES;
	for (i = target; i > 0; i--)
	{
		char c = data[i - 1];

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
			return window_start + i;
	}

	pos = 0;
	while (pos < target)
	{
		int mblen = pg_mblen(data + pos);

		if (mblen <= 0)
			mblen = 1;
		if (pos + mblen > target)
			break;
		pos += mblen;
	}
	if (pos == 0)
	{
		pos = pg_mblen(data);
		if (pos <= 0)
			pos = 1;
	}

	return window_start + pos;
}

static void
tp_emit_parsed_word(
		void  *context,
		char  *lexeme,
		int	   length,
		int	   global_position,
		int	   window_id,
		int	   window_position,
		uint16 flags,
		uint16 variant)
{
	TpParsedTextSink *sink	 = context;
	ParsedText		 *parsed = sink->parsed;

	(void)window_id;
	(void)window_position;

	if (parsed->curwords == parsed->lenwords)
	{
		parsed->lenwords *= 2;
		parsed->words =
				repalloc(parsed->words, parsed->lenwords * sizeof(ParsedWord));
	}

	parsed->words[parsed->curwords].len		 = length;
	parsed->words[parsed->curwords].word	 = lexeme;
	parsed->words[parsed->curwords].nvariant = variant;
	parsed->words[parsed->curwords].flags	 = flags & TSL_PREFIX;
	parsed->words[parsed->curwords].alen	 = 0;
	parsed->words[parsed->curwords].pos.pos	 = LIMITPOS(global_position);
	parsed->curwords++;
}

static uint32
tp_term_frequency_hash(const void *key, Size keysize)
{
	const char *term = *(const char *const *)key;

	(void)keysize;
	return DatumGetUInt32(hash_any((const unsigned char *)term, strlen(term)));
}

static int
tp_term_frequency_match(const void *key1, const void *key2, Size keysize)
{
	const char *term1 = *(const char *const *)key1;
	const char *term2 = *(const char *const *)key2;

	(void)keysize;
	return strcmp(term1, term2);
}

static void
tp_emit_term_frequency(
		void  *context,
		char  *lexeme,
		int	   length,
		int	   global_position,
		int	   window_id,
		int	   window_position,
		uint16 flags,
		uint16 variant)
{
	TpTermFrequencySink	 *sink = context;
	TpTermFrequencyEntry *entry;
	bool				  found;
	int					  limited_position = LIMITPOS(window_position);

	(void)length;
	(void)global_position;
	(void)flags;
	(void)variant;

	entry = hash_search(sink->terms, &lexeme, HASH_ENTER, &found);
	if (!found)
	{
		entry->term				= lexeme;
		entry->frequency		= 1;
		entry->window_frequency = 1;
		entry->last_window		= window_id;
		entry->last_position	= limited_position;
		return;
	}

	pfree(lexeme);
	if (entry->last_window != window_id)
	{
		entry->frequency++;
		entry->window_frequency = 1;
		entry->last_window		= window_id;
		entry->last_position	= limited_position;
	}
	else if (
			entry->window_frequency < MAXNUMPOS - 1 &&
			entry->last_position != MAXENTRYPOS - 1 &&
			entry->last_position != limited_position)
	{
		entry->frequency++;
		entry->window_frequency++;
		entry->last_position = limited_position;
	}
}

static void
tp_parse_text_to_sink(
		Oid			 config_oid,
		char		*input,
		int			 input_length,
		int			 max_token_length,
		bool		*normalization_changed,
		TpLexemeSink sink,
		void		*sink_context)
{
	TSConfigCacheEntry *config;
	TSParserCacheEntry *parser;
	TpLexizeData		lexize;
	void			   *parser_state;
	int					type;
	int					token_length	 = 0;
	char			   *token			 = NULL;
	int					input_window	 = 0;
	int					input_window_end = input_length;
	int					output_window	 = 0;
	int					global_position	 = 0;
	int					window_position	 = 0;

	config		 = lookup_ts_config_cache(config_oid);
	parser		 = lookup_ts_parser_cache(config->prsId);
	parser_state = DatumGetPointer(FunctionCall2(
			&parser->prsstart,
			PointerGetDatum(input),
			Int32GetDatum(input_length)));
	tp_lexize_init(&lexize, config);
	if (input_length > 0)
		input_window_end = tp_token_window_end(input, input_length, 0);

	do
	{
		TSLexeme	*normalized;
		TpParsedLex *corresponding;

		type = DatumGetInt32(FunctionCall3(
				&parser->prstoken,
				PointerGetDatum(parser_state),
				PointerGetDatum(&token),
				PointerGetDatum(&token_length)));

		if (type > 0)
		{
			int original_length = token_length;
			int token_offset	= token - input;

			Assert(token_offset >= 0 && token_offset < input_length);
			while (token_offset >= input_window_end &&
				   input_window_end < input_length)
			{
				input_window++;
				input_window_end = tp_token_window_end(
						input, input_length, input_window_end);
			}

			token_length = tp_clip_token_length(
					token, token_length, max_token_length);
			if (normalization_changed != NULL &&
				token_length < original_length)
				*normalization_changed = true;
			if (token_length == 0)
				continue;
		}

		tp_lexize_add(&lexize, type, token, token_length, input_window);
		for (;;)
		{
			TSLexeme *current;
			int		  result_window;

			corresponding = NULL;
			result_window = input_window;
			normalized =
					tp_lexize_exec(&lexize, &corresponding, &result_window);
			if (normalized == NULL)
			{
				tp_parsed_lex_list_free(corresponding);
				break;
			}

			/*
			 * A getnext dictionary can emit only after consuming tokens from
			 * later windows.  Charge the result to the first token that
			 * started that normalization, while keeping dictionary state
			 * continuous across the virtual boundary.
			 */
			Assert(result_window >= output_window);
			if (result_window != output_window)
			{
				output_window	= result_window;
				window_position = 0;
			}

			global_position++;
			window_position++;
			for (current = normalized; current->lexeme; current++)
			{
				int lexeme_length  = strlen(current->lexeme);
				int clipped_length = tp_clip_token_length(
						current->lexeme, lexeme_length, max_token_length);

				if (normalization_changed != NULL &&
					clipped_length < lexeme_length)
					*normalization_changed = true;
				if (clipped_length == 0)
				{
					pfree(current->lexeme);
					continue;
				}
				if (clipped_length < lexeme_length)
				{
					char *clipped = pnstrdup(current->lexeme, clipped_length);

					pfree(current->lexeme);
					current->lexeme = clipped;
				}

				if (current->flags & TSL_ADDPOS)
				{
					global_position++;
					window_position++;
				}
				sink(sink_context,
					 current->lexeme,
					 clipped_length,
					 global_position,
					 result_window,
					 window_position,
					 current->flags,
					 current->nvariant);
			}
			pfree(normalized);
			tp_parsed_lex_list_free(corresponding);
		}
	} while (type > 0);

	FunctionCall1(&parser->prsend, PointerGetDatum(parser_state));
}

TSVector
tp_make_tsvector(
		text *input,
		Oid	  text_config_oid,
		int	  max_token_length,
		bool *normalization_changed)
{
	ParsedText		 parsed;
	TpParsedTextSink sink;

	if (normalization_changed != NULL)
		*normalization_changed = false;

	if (max_token_length == 0)
		return DatumGetTSVector(DirectFunctionCall2Coll(
				to_tsvector_byid,
				InvalidOid,
				ObjectIdGetDatum(text_config_oid),
				PointerGetDatum(input)));

	/*
	 * Grow with emitted parser output rather than preallocating from raw
	 * input size.  Large single tokens and punctuation-heavy inputs can have
	 * very different input and output cardinalities.
	 */
	parsed.lenwords = 32;
	parsed.curwords = 0;
	parsed.pos		= 0;
	parsed.words	= palloc(sizeof(ParsedWord) * parsed.lenwords);
	sink.parsed		= &parsed;

	tp_parse_text_to_sink(
			text_config_oid,
			VARDATA_ANY(input),
			VARSIZE_ANY_EXHDR(input),
			max_token_length,
			normalization_changed,
			tp_emit_parsed_word,
			&sink);

	return make_tsvector(&parsed);
}

static int
tp_term_frequency_entry_cmp(const void *left, const void *right)
{
	const TpTermFrequencyEntry *a = left;
	const TpTermFrequencyEntry *b = right;

	return strcmp(a->term, b->term);
}

int
tp_tokenize_document(
		text   *input,
		Oid		text_config_oid,
		int		max_token_length,
		bool   *normalization_changed,
		char ***terms_out,
		int32 **frequencies_out,
		int	   *term_count_out)
{
	HASHCTL				  info;
	TpTermFrequencySink	  sink;
	HASH_SEQ_STATUS		  status;
	TpTermFrequencyEntry *entry;
	TpTermFrequencyEntry *entries;
	char				**terms;
	int32				 *frequencies;
	int					  count;
	int					  i;
	int					  doc_length = 0;

	Assert(max_token_length > 0);

	if (normalization_changed != NULL)
		*normalization_changed = false;

	memset(&info, 0, sizeof(info));
	info.keysize   = sizeof(char *);
	info.entrysize = sizeof(TpTermFrequencyEntry);
	info.hash	   = tp_term_frequency_hash;
	info.match	   = tp_term_frequency_match;
	sink.terms	   = hash_create(
			"pg_textsearch token frequencies",
			1024,
			&info,
			HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	tp_parse_text_to_sink(
			text_config_oid,
			VARDATA_ANY(input),
			VARSIZE_ANY_EXHDR(input),
			max_token_length,
			normalization_changed,
			tp_emit_term_frequency,
			&sink);

	count = hash_get_num_entries(sink.terms);
	if (count == 0)
	{
		hash_destroy(sink.terms);
		*terms_out		 = NULL;
		*frequencies_out = NULL;
		*term_count_out	 = 0;
		return 0;
	}

	entries = palloc(count * sizeof(TpTermFrequencyEntry));
	i		= 0;
	hash_seq_init(&status, sink.terms);
	while ((entry = hash_seq_search(&status)) != NULL)
		entries[i++] = *entry;
	Assert(i == count);
	hash_destroy(sink.terms);

	qsort(entries,
		  count,
		  sizeof(TpTermFrequencyEntry),
		  tp_term_frequency_entry_cmp);
	terms		= palloc(count * sizeof(char *));
	frequencies = palloc(count * sizeof(int32));
	for (i = 0; i < count; i++)
	{
		terms[i]	   = entries[i].term;
		frequencies[i] = entries[i].frequency;
		doc_length += entries[i].frequency;
	}
	pfree(entries);

	*terms_out		 = terms;
	*frequencies_out = frequencies;
	*term_count_out	 = count;
	return doc_length;
}
