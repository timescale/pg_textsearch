/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * Boolean query execution over BM25 posting lists.
 */
#include <postgres.h>

#include <access/relscan.h>
#include <miscadmin.h>
#include <tsearch/ts_cache.h>
#include <tsearch/ts_utils.h>
#include <utils/fmgrprotos.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>

#include "access/am.h"
#include "access/boolean.h"
#include "index/source.h"
#include "memtable/cache_source.h"
#include "segment/alive_bitset.h"
#include "segment/io.h"

typedef struct TpBooleanTerm
{
	char *lexeme;
	int	  length;
	HTAB *ctids;
} TpBooleanTerm;

typedef struct TpBooleanEvalState
{
	TSQuery		   query;
	TpBooleanTerm *terms;
	int			   term_count;
	bool		   requires_recheck;
	bool		   requires_all_docs;
} TpBooleanEvalState;

static HTAB *
tp_boolean_create_ctid_set(const char *name, long initial_size)
{
	HASHCTL ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize	  = sizeof(ItemPointerData);
	ctl.entrysize = sizeof(ItemPointerData);
	ctl.hcxt	  = CurrentMemoryContext;

	return hash_create(
			name, initial_size, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void
tp_boolean_check_config(Relation index, TpIndexMetaPage metap)
{
	Oid current_config = getTSCurrentConfig(true);

	if (metap->text_config_oid != current_config)
	{
		char *current_config_name = DatumGetCString(DirectFunctionCall1(
				regconfigout, ObjectIdGetDatum(current_config)));
		char *index_config_name	  = DatumGetCString(DirectFunctionCall1(
				  regconfigout, ObjectIdGetDatum(metap->text_config_oid)));

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("BM25 index \"%s\" cannot evaluate text search "
						"configuration \"%s\"",
						RelationGetRelationName(index),
						current_config_name),
				 errdetail(
						 "The index uses text search configuration \"%s\".",
						 index_config_name),
				 errhint("Set default_text_search_config to match the index "
						 "or use a different index.")));
	}
}

static TpBooleanTerm *
tp_boolean_find_term(TpBooleanEvalState *state, const char *lexeme, int length)
{
	for (int i = 0; i < state->term_count; i++)
	{
		TpBooleanTerm *term = &state->terms[i];

		if (term->length == length &&
			memcmp(term->lexeme, lexeme, length) == 0)
			return term;
	}

	return NULL;
}

static TpBooleanEvalState
tp_boolean_extract_terms(TSQuery query)
{
	TpBooleanEvalState state;
	QueryItem		  *items	= GETQUERY(query);
	char			  *operands = GETOPERAND(query);

	memset(&state, 0, sizeof(state));
	state.query = query;
	state.terms = palloc0(Max(query->size, 1) * sizeof(TpBooleanTerm));

	for (int i = 0; i < query->size; i++)
	{
		QueryItem *item = &items[i];

		if (item->type == QI_OPR)
		{
			if (item->qoperator.oper == OP_PHRASE)
				state.requires_recheck = true;
			continue;
		}

		if (item->type == QI_VAL)
		{
			QueryOperand  *operand = &item->qoperand;
			const char	  *lexeme  = operands + operand->distance;
			TpBooleanTerm *term;

			/*
			 * Postings do not retain lexeme weights and the index cannot
			 * enumerate dictionary prefixes yet.  Treat these operands as
			 * unknown over an all-document candidate set and let the heap
			 * recheck evaluate their exact semantics.
			 */
			if (operand->prefix || operand->weight != 0)
			{
				state.requires_recheck	= true;
				state.requires_all_docs = true;
				continue;
			}

			if (tp_boolean_find_term(&state, lexeme, operand->length) != NULL)
				continue;

			term		 = &state.terms[state.term_count++];
			term->length = operand->length;
			term->lexeme = pnstrdup(lexeme, operand->length);
			term->ctids =
					tp_boolean_create_ctid_set("BM25 Boolean term CTIDs", 256);
		}
	}

	state.requires_all_docs |= !tsquery_requires_match(GETQUERY(query));

	return state;
}

static void
tp_boolean_add_ctid(TpBooleanTerm *term, HTAB *candidates, ItemPointer ctid)
{
	bool found;

	(void)hash_search(term->ctids, ctid, HASH_ENTER, &found);
	if (candidates != NULL)
		(void)hash_search(candidates, ctid, HASH_ENTER, &found);
}

static void
tp_boolean_collect_memtable(
		TpBooleanEvalState *state, TpDataSource *source, HTAB *candidates)
{
	if (source == NULL)
		return;

	for (int i = 0; i < state->term_count; i++)
	{
		TpBooleanTerm *term		= &state->terms[i];
		TpPostingData *postings = tp_source_get_postings(source, term->lexeme);

		if (postings == NULL)
			continue;

		for (int j = 0; j < postings->count; j++)
			tp_boolean_add_ctid(term, candidates, &postings->ctids[j]);

		tp_source_free_postings(source, postings);
	}
}

static void
tp_boolean_collect_segment(
		TpBooleanEvalState *state, TpSegmentReader *reader, HTAB *candidates)
{
	for (int i = 0; i < state->term_count; i++)
	{
		TpBooleanTerm			*term = &state->terms[i];
		TpSegmentPostingIterator iter;
		TpSegmentPosting		*posting;

		if (!tp_segment_posting_iterator_init(&iter, reader, term->lexeme))
			continue;

		while (tp_segment_posting_iterator_next(&iter, &posting))
		{
			ItemPointerData ctid;

			CHECK_FOR_INTERRUPTS();

			if (!tp_segment_is_alive(reader, posting->doc_id))
				continue;

			tp_segment_lookup_ctid(reader, posting->doc_id, &ctid);
			if (ItemPointerIsValid(&ctid))
				tp_boolean_add_ctid(term, candidates, &ctid);
		}

		tp_segment_posting_iterator_free(&iter);
	}
}

typedef struct TpBooleanCandidateEval
{
	TpBooleanEvalState *query;
	ItemPointer			ctid;
} TpBooleanCandidateEval;

typedef struct TpBooleanResultWriter
{
	TpBooleanEvalState *query;
	BufFile			   *file;
	int					count;
} TpBooleanResultWriter;

static TSTernaryValue
tp_boolean_candidate_has_term(
		void *arg, QueryOperand *operand, ExecPhraseData *data)
{
	TpBooleanCandidateEval *eval = arg;
	const char	  *lexeme = GETOPERAND(eval->query->query) + operand->distance;
	TpBooleanTerm *term =
			tp_boolean_find_term(eval->query, lexeme, operand->length);

	if (operand->prefix || operand->weight != 0)
		return TS_MAYBE;

	Assert(term != NULL);
	if (hash_search(term->ctids, eval->ctid, HASH_FIND, NULL) == NULL)
		return TS_NO;

	return data == NULL ? TS_YES : TS_MAYBE;
}

static bool
tp_boolean_candidate_matches(TpBooleanEvalState *state, ItemPointer candidate)
{
	TpBooleanCandidateEval eval = {
			.query = state,
			.ctid  = candidate,
	};

	return TS_execute_ternary(
				   GETQUERY(state->query),
				   &eval,
				   TS_EXEC_PHRASE_NO_POS,
				   tp_boolean_candidate_has_term) != TS_NO;
}

static void
tp_boolean_write_candidate(ItemPointer candidate, void *arg)
{
	TpBooleanResultWriter *writer = arg;

	CHECK_FOR_INTERRUPTS();

	if (!ItemPointerIsValid(candidate) ||
		!tp_boolean_candidate_matches(writer->query, candidate))
		return;

	BufFileWrite(writer->file, candidate, sizeof(ItemPointerData));
	writer->count++;
}

static void
tp_boolean_write_all_segment_docs(
		TpSegmentReader *reader, TpBooleanResultWriter *writer)
{
	for (uint32 doc_id = 0; doc_id < reader->header->num_docs; doc_id++)
	{
		ItemPointerData ctid;

		CHECK_FOR_INTERRUPTS();

		if (!tp_segment_is_alive(reader, doc_id))
			continue;

		tp_segment_lookup_ctid(reader, doc_id, &ctid);
		tp_boolean_write_candidate(&ctid, writer);
	}
}

void
tp_boolean_rescan(
		IndexScanDesc scan, ScanKey keys, int nkeys, TpIndexMetaPage metap)
{
	TpScanOpaque  so = (TpScanOpaque)scan->opaque;
	MemoryContext old_context;

	if (nkeys != 1 || keys == NULL ||
		keys[0].sk_strategy != TSearchStrategyNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BM25 Boolean scans require exactly one @@ "
						"condition")));

	MemoryContextReset(so->boolean_context);
	so->boolean_query	= NULL;
	so->is_boolean_scan = true;
	so->boolean_recheck = false;

	if (keys[0].sk_flags & SK_ISNULL)
		return;

	tp_boolean_check_config(scan->indexRelation, metap);

	old_context		  = MemoryContextSwitchTo(so->boolean_context);
	so->boolean_query = DatumGetTSQueryCopy(keys[0].sk_argument);
	MemoryContextSwitchTo(old_context);
}

bool
tp_boolean_execute(
		IndexScanDesc	   scan,
		TpLocalIndexState *index_state,
		TpIndexMetaPage	   metap)
{
	TpScanOpaque	   so = (TpScanOpaque)scan->opaque;
	MemoryContext	   old_context;
	TpBooleanEvalState state;
	const char		 **terms;
	TpDataSource	  *memtable_source;
	HTAB			  *candidates;
	HASH_SEQ_STATUS	   sequence;
	ItemPointer		   candidate;
	int				   result_count = 0;
	int				   capacity;

	if (so->boolean_query == NULL || so->boolean_query->size == 0)
		return false;

	tp_boolean_check_config(scan->indexRelation, metap);

	old_context			= MemoryContextSwitchTo(so->boolean_context);
	state				= tp_boolean_extract_terms(so->boolean_query);
	terms				= palloc(state.term_count * sizeof(char *));
	candidates			= state.requires_all_docs
								? NULL
								: tp_boolean_create_ctid_set(
								  "BM25 Boolean candidates", 1024);
	so->boolean_recheck = state.requires_recheck;

	for (int i = 0; i < state.term_count; i++)
		terms[i] = state.terms[i].lexeme;

	memtable_source = tp_memtable_source_create_for_read(
			index_state, scan->indexRelation, terms, state.term_count);
	tp_boolean_collect_memtable(&state, memtable_source, candidates);

	for (int level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber segment = metap->level_heads[level];

		while (segment != InvalidBlockNumber)
		{
			TpSegmentReader *reader =
					tp_segment_open(scan->indexRelation, segment);

			tp_boolean_collect_segment(&state, reader, candidates);
			segment = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}

	if (state.requires_all_docs)
	{
		TpBooleanResultWriter writer = {
				.query = &state,
				.file  = BufFileCreateTemp(false),
		};

		if (memtable_source != NULL)
			tp_source_foreach_document(
					memtable_source, tp_boolean_write_candidate, &writer);

		for (int level = 0; level < TP_MAX_LEVELS; level++)
		{
			BlockNumber segment = metap->level_heads[level];

			while (segment != InvalidBlockNumber)
			{
				TpSegmentReader *reader =
						tp_segment_open(scan->indexRelation, segment);

				tp_boolean_write_all_segment_docs(reader, &writer);
				segment = reader->header->next_segment;
				tp_segment_close(reader);
			}
		}

		so->boolean_results = writer.file;
		so->result_count	= writer.count;
		so->current_pos		= 0;
		if (BufFileSeek(writer.file, 0, 0, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind BM25 Boolean result file")));

		if (memtable_source != NULL)
			tp_source_close(memtable_source);

		MemoryContextSwitchTo(old_context);
		return writer.count > 0;
	}

	if (memtable_source != NULL)
		tp_source_close(memtable_source);

	capacity = hash_get_num_entries(candidates);
	MemoryContextSwitchTo(so->scan_context);
	so->result_ctids = capacity > 0
							 ? palloc(capacity * sizeof(ItemPointerData))
							 : NULL;
	MemoryContextSwitchTo(so->boolean_context);

	hash_seq_init(&sequence, candidates);
	while ((candidate = hash_seq_search(&sequence)) != NULL)
	{
		if (tp_boolean_candidate_matches(&state, candidate))
			so->result_ctids[result_count++] = *candidate;
	}

	so->result_count = result_count;
	so->current_pos	 = 0;
	MemoryContextSwitchTo(old_context);

	return result_count > 0;
}

bool
tp_boolean_next(IndexScanDesc scan)
{
	TpScanOpaque	so = (TpScanOpaque)scan->opaque;
	ItemPointerData ctid;
	size_t			bytes_read;

	Assert(so->boolean_results != NULL);

	bytes_read =
			BufFileRead(so->boolean_results, &ctid, sizeof(ItemPointerData));
	if (bytes_read == 0)
		return false;
	if (bytes_read != sizeof(ItemPointerData))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read BM25 Boolean result file")));

	scan->xs_heaptid = ctid;
	so->current_pos++;
	return true;
}
