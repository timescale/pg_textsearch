/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * The parser/dictionary pipeline below is adapted from PostgreSQL's
 * src/backend/tsearch/ts_parse.c, Copyright (c) 1996-2025 PostgreSQL
 * Global Development Group, and is used under the PostgreSQL License.
 */
#include <postgres.h>

#include <mb/pg_wchar.h>
#include <tsearch/ts_cache.h>
#include <tsearch/ts_utils.h>
#include <utils/fmgrprotos.h>
#include <varatt.h>

#include "types/tokenize.h"

typedef struct TpParsedLex
{
	int					type;
	char			   *lemm;
	int					lenlemm;
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
} TpLexizeData;

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
tp_lexize_add(TpLexizeData *state, int type, char *lexeme, int length)
{
	TpParsedLex *parsed = palloc(sizeof(*parsed));

	parsed->type	= type;
	parsed->lemm	= lexeme;
	parsed->lenlemm = length;
	tp_parsed_lex_list_append(&state->work, parsed);
	state->current_sublexeme = state->work.tail;
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
tp_lexize_exec(TpLexizeData *state, TpParsedLex **corresponding)
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

			map = state->cfg->map + current->type;
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
					return tp_lexize_exec(state, corresponding);
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
					return tp_lexize_exec(state, corresponding);
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
				return result;
			}

			state->cur_dict_id = InvalidOid;
			return tp_lexize_exec(state, corresponding);
		}
	}

	tp_lexize_set_corresponding(state, corresponding);
	return NULL;
}

static int
tp_clip_token_length(const char *token, int length, int max_token_length)
{
	if (length <= max_token_length)
		return length;

	return pg_mbcliplen(token, length, max_token_length);
}

static void
tp_parse_text(
		Oid			config_oid,
		ParsedText *parsed,
		char	   *input,
		int			input_length,
		int			max_token_length)
{
	TSConfigCacheEntry *config;
	TSParserCacheEntry *parser;
	TpLexizeData		lexize;
	void			   *parser_state;
	int					type;
	int					token_length = 0;
	char			   *token		 = NULL;

	config		 = lookup_ts_config_cache(config_oid);
	parser		 = lookup_ts_parser_cache(config->prsId);
	parser_state = DatumGetPointer(FunctionCall2(
			&parser->prsstart,
			PointerGetDatum(input),
			Int32GetDatum(input_length)));
	tp_lexize_init(&lexize, config);

	do
	{
		TSLexeme *normalized;

		type = DatumGetInt32(FunctionCall3(
				&parser->prstoken,
				PointerGetDatum(parser_state),
				PointerGetDatum(&token),
				PointerGetDatum(&token_length)));

		if (type > 0)
		{
			token_length = tp_clip_token_length(
					token, token_length, max_token_length);
			if (token_length == 0)
				continue;
		}

		tp_lexize_add(&lexize, type, token, token_length);
		while ((normalized = tp_lexize_exec(&lexize, NULL)) != NULL)
		{
			TSLexeme *current;

			parsed->pos++;
			for (current = normalized; current->lexeme; current++)
			{
				int lexeme_length  = strlen(current->lexeme);
				int clipped_length = tp_clip_token_length(
						current->lexeme, lexeme_length, max_token_length);

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

				if (parsed->curwords == parsed->lenwords)
				{
					parsed->lenwords *= 2;
					parsed->words = repalloc(
							parsed->words,
							parsed->lenwords * sizeof(ParsedWord));
				}

				if (current->flags & TSL_ADDPOS)
					parsed->pos++;
				parsed->words[parsed->curwords].len		 = clipped_length;
				parsed->words[parsed->curwords].word	 = current->lexeme;
				parsed->words[parsed->curwords].nvariant = current->nvariant;
				parsed->words[parsed->curwords].flags	 = current->flags &
														TSL_PREFIX;
				parsed->words[parsed->curwords].alen	= 0;
				parsed->words[parsed->curwords].pos.pos = LIMITPOS(
						parsed->pos);
				parsed->curwords++;
			}
			pfree(normalized);
		}
	} while (type > 0);

	FunctionCall1(&parser->prsend, PointerGetDatum(parser_state));
}

TSVector
tp_make_tsvector(text *input, Oid text_config_oid, int max_token_length)
{
	ParsedText parsed;

	if (max_token_length == 0)
		return DatumGetTSVector(DirectFunctionCall2Coll(
				to_tsvector_byid,
				InvalidOid,
				ObjectIdGetDatum(text_config_oid),
				PointerGetDatum(input)));

	parsed.lenwords = VARSIZE_ANY_EXHDR(input) / 6;
	if (parsed.lenwords < 2)
		parsed.lenwords = 2;
	else if ((Size)parsed.lenwords > MaxAllocSize / sizeof(ParsedWord))
		parsed.lenwords = (int32)(MaxAllocSize / sizeof(ParsedWord));
	parsed.curwords = 0;
	parsed.pos		= 0;
	parsed.words	= palloc(sizeof(ParsedWord) * parsed.lenwords);

	tp_parse_text(
			text_config_oid,
			&parsed,
			VARDATA_ANY(input),
			VARSIZE_ANY_EXHDR(input),
			max_token_length);

	return make_tsvector(&parsed);
}
