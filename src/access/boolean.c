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

typedef enum TpBooleanCandidateKind
{
	TP_BOOLEAN_CANDIDATE_ALL,
	TP_BOOLEAN_CANDIDATE_TERM,
	TP_BOOLEAN_CANDIDATE_UNION,
} TpBooleanCandidateKind;

typedef struct TpBooleanCandidateStream
{
	TpBooleanCandidateKind kind;
	uint64				   estimate;

	union
	{
		struct
		{
			uint32 next_doc_id;
			uint32 num_docs;
		} all;

		struct
		{
			TpSegmentPostingIterator iterator;
			bool					 initialized;
		} term;

		struct
		{
			struct TpBooleanCandidateStream *left;
			struct TpBooleanCandidateStream *right;
			uint32							 left_doc_id;
			uint32							 right_doc_id;
			bool							 left_loaded;
			bool							 right_loaded;
			bool							 left_valid;
			bool							 right_valid;
		} union_stream;
	} state;
} TpBooleanCandidateStream;

typedef struct TpBooleanTermCursor
{
	TpSegmentPostingIterator iterator;
	TpSegmentPosting		*posting;
	bool					 initialized;
	bool					 positioned;
} TpBooleanTermCursor;

typedef struct TpBooleanSegmentEval
{
	TpBooleanEvalState	*query;
	TpBooleanTermCursor *cursors;
	uint32				 doc_id;
} TpBooleanSegmentEval;

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

static TpBooleanCandidateStream *
tp_boolean_create_all_stream(TpSegmentReader *reader)
{
	TpBooleanCandidateStream *stream = palloc0(sizeof(*stream));

	stream->kind			   = TP_BOOLEAN_CANDIDATE_ALL;
	stream->estimate		   = reader->header->num_docs;
	stream->state.all.num_docs = reader->header->num_docs;
	return stream;
}

static void tp_boolean_free_candidate_stream(TpBooleanCandidateStream *stream);

static TpBooleanCandidateStream *
tp_boolean_create_candidate_stream(
		TpBooleanEvalState *state, TpSegmentReader *reader, QueryItem *item)
{
	TpBooleanCandidateStream *stream;

	if (item->type == QI_VAL)
	{
		QueryOperand *operand = &item->qoperand;
		const char	 *lexeme  = GETOPERAND(state->query) + operand->distance;

		if (operand->prefix || operand->weight != 0)
			return tp_boolean_create_all_stream(reader);

		stream						   = palloc0(sizeof(*stream));
		stream->kind				   = TP_BOOLEAN_CANDIDATE_TERM;
		stream->state.term.initialized = tp_segment_posting_iterator_init(
				&stream->state.term.iterator, reader, lexeme);
		stream->estimate =
				stream->state.term.initialized
						? stream->state.term.iterator.dict_entry.doc_freq
						: 0;
		return stream;
	}

	if (item->type != QI_OPR)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid item in BM25 Boolean query")));

	if (item->qoperator.oper == OP_NOT)
		return tp_boolean_create_all_stream(reader);

	{
		TpBooleanCandidateStream *right =
				tp_boolean_create_candidate_stream(state, reader, item + 1);
		TpBooleanCandidateStream *left = tp_boolean_create_candidate_stream(
				state, reader, item + item->qoperator.left);

		if (item->qoperator.oper == OP_AND ||
			item->qoperator.oper == OP_PHRASE)
		{
			if (left->estimate <= right->estimate)
			{
				tp_boolean_free_candidate_stream(right);
				return left;
			}

			tp_boolean_free_candidate_stream(left);
			return right;
		}

		if (item->qoperator.oper != OP_OR)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("invalid operator in BM25 Boolean query")));

		stream		 = palloc0(sizeof(*stream));
		stream->kind = TP_BOOLEAN_CANDIDATE_UNION;
		stream->estimate =
				Min((uint64)reader->header->num_docs,
					left->estimate + right->estimate);
		stream->state.union_stream.left	 = left;
		stream->state.union_stream.right = right;
		return stream;
	}
}

static bool
tp_boolean_candidate_stream_next(
		TpBooleanCandidateStream *stream, uint32 *doc_id)
{
	switch (stream->kind)
	{
	case TP_BOOLEAN_CANDIDATE_ALL:
		if (stream->state.all.next_doc_id >= stream->state.all.num_docs)
			return false;
		*doc_id = stream->state.all.next_doc_id++;
		return true;

	case TP_BOOLEAN_CANDIDATE_TERM:
	{
		TpSegmentPosting *posting;

		if (!stream->state.term.initialized ||
			!tp_segment_posting_iterator_next(
					&stream->state.term.iterator, &posting))
			return false;
		*doc_id = posting->doc_id;
		return true;
	}

	case TP_BOOLEAN_CANDIDATE_UNION:
	{
		TpBooleanCandidateStream *left	= stream->state.union_stream.left;
		TpBooleanCandidateStream *right = stream->state.union_stream.right;

		if (!stream->state.union_stream.left_loaded)
		{
			stream->state.union_stream.left_valid =
					tp_boolean_candidate_stream_next(
							left, &stream->state.union_stream.left_doc_id);
			stream->state.union_stream.left_loaded = true;
		}
		if (!stream->state.union_stream.right_loaded)
		{
			stream->state.union_stream.right_valid =
					tp_boolean_candidate_stream_next(
							right, &stream->state.union_stream.right_doc_id);
			stream->state.union_stream.right_loaded = true;
		}

		if (!stream->state.union_stream.left_valid &&
			!stream->state.union_stream.right_valid)
			return false;

		if (!stream->state.union_stream.right_valid ||
			(stream->state.union_stream.left_valid &&
			 stream->state.union_stream.left_doc_id <=
					 stream->state.union_stream.right_doc_id))
			*doc_id = stream->state.union_stream.left_doc_id;
		else
			*doc_id = stream->state.union_stream.right_doc_id;

		if (stream->state.union_stream.left_valid &&
			stream->state.union_stream.left_doc_id == *doc_id)
			stream->state.union_stream.left_loaded = false;
		if (stream->state.union_stream.right_valid &&
			stream->state.union_stream.right_doc_id == *doc_id)
			stream->state.union_stream.right_loaded = false;
		return true;
	}
	}

	pg_unreachable();
}

static void
tp_boolean_free_candidate_stream(TpBooleanCandidateStream *stream)
{
	if (stream == NULL)
		return;

	if (stream->kind == TP_BOOLEAN_CANDIDATE_TERM &&
		stream->state.term.initialized)
		tp_segment_posting_iterator_free(&stream->state.term.iterator);
	else if (stream->kind == TP_BOOLEAN_CANDIDATE_UNION)
	{
		tp_boolean_free_candidate_stream(stream->state.union_stream.left);
		tp_boolean_free_candidate_stream(stream->state.union_stream.right);
	}

	pfree(stream);
}

static TSTernaryValue
tp_boolean_segment_has_term(
		void *arg, QueryOperand *operand, ExecPhraseData *data)
{
	TpBooleanSegmentEval *eval = arg;
	const char	  *lexeme = GETOPERAND(eval->query->query) + operand->distance;
	TpBooleanTerm *term =
			tp_boolean_find_term(eval->query, lexeme, operand->length);
	TpBooleanTermCursor *cursor;

	if (operand->prefix || operand->weight != 0)
		return TS_MAYBE;

	Assert(term != NULL);
	cursor = &eval->cursors[term - eval->query->terms];
	if (!cursor->initialized)
		return TS_NO;

	if (!cursor->positioned)
	{
		cursor->positioned = tp_segment_posting_iterator_seek(
				&cursor->iterator, eval->doc_id, &cursor->posting);
		if (!cursor->positioned)
			return TS_NO;
	}
	else if (cursor->posting->doc_id < eval->doc_id)
	{
		if (eval->doc_id > cursor->iterator.skip_entry.last_doc_id)
			cursor->positioned = tp_segment_posting_iterator_seek(
					&cursor->iterator, eval->doc_id, &cursor->posting);
		else
		{
			do
			{
				cursor->positioned = tp_segment_posting_iterator_next(
						&cursor->iterator, &cursor->posting);
			} while (cursor->positioned &&
					 cursor->posting->doc_id < eval->doc_id);
		}

		if (!cursor->positioned)
			return TS_NO;
	}

	if (cursor->posting->doc_id != eval->doc_id)
		return TS_NO;

	return data == NULL ? TS_YES : TS_MAYBE;
}

static bool
tp_boolean_segment_candidate_matches(
		TpBooleanEvalState *state, TpBooleanTermCursor *cursors, uint32 doc_id)
{
	TpBooleanSegmentEval eval = {
			.query	 = state,
			.cursors = cursors,
			.doc_id	 = doc_id,
	};

	return TS_execute_ternary(
				   GETQUERY(state->query),
				   &eval,
				   TS_EXEC_PHRASE_NO_POS,
				   tp_boolean_segment_has_term) != TS_NO;
}

static void
tp_boolean_write_result(TpBooleanResultWriter *writer, ItemPointer candidate)
{
	BufFileWrite(writer->file, candidate, sizeof(ItemPointerData));
	writer->count++;
}

static void
tp_boolean_write_candidate(ItemPointer candidate, void *arg)
{
	TpBooleanResultWriter *writer = arg;

	CHECK_FOR_INTERRUPTS();

	if (!ItemPointerIsValid(candidate) ||
		!tp_boolean_candidate_matches(writer->query, candidate))
		return;

	tp_boolean_write_result(writer, candidate);
}

static void
tp_boolean_write_segment(
		TpSegmentReader *reader, TpBooleanResultWriter *writer)
{
	TpBooleanCandidateStream *stream = tp_boolean_create_candidate_stream(
			writer->query, reader, GETQUERY(writer->query->query));
	TpBooleanTermCursor *cursors = palloc0(
			writer->query->term_count * sizeof(*cursors));
	uint32 cache_threshold = reader->header->num_docs / 100 +
							 (reader->header->num_docs % 100 != 0);
	uint32 doc_id;

	if (stream->estimate >= cache_threshold)
		tp_segment_enable_ctid_lookup_cache(reader);

	for (int i = 0; i < writer->query->term_count; i++)
		cursors[i].initialized = tp_segment_posting_iterator_init(
				&cursors[i].iterator, reader, writer->query->terms[i].lexeme);

	while (tp_boolean_candidate_stream_next(stream, &doc_id))
	{
		ItemPointerData ctid;

		CHECK_FOR_INTERRUPTS();

		if (!tp_segment_is_alive(reader, doc_id))
			continue;
		if (!tp_boolean_segment_candidate_matches(
					writer->query, cursors, doc_id))
			continue;

		tp_segment_lookup_ctid(reader, doc_id, &ctid);
		if (ItemPointerIsValid(&ctid))
			tp_boolean_write_result(writer, &ctid);
	}

	for (int i = 0; i < writer->query->term_count; i++)
	{
		if (cursors[i].initialized)
			tp_segment_posting_iterator_free(&cursors[i].iterator);
	}
	pfree(cursors);
	tp_boolean_free_candidate_stream(stream);
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
	TpScanOpaque		  so = (TpScanOpaque)scan->opaque;
	MemoryContext		  old_context;
	TpBooleanEvalState	  state;
	const char			**terms;
	TpDataSource		 *memtable_source;
	HTAB				 *candidates;
	HASH_SEQ_STATUS		  sequence;
	ItemPointer			  candidate;
	TpBooleanResultWriter writer;

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

	writer.query = &state;
	writer.file	 = BufFileCreateTemp(false);
	writer.count = 0;

	if (state.requires_all_docs)
	{
		if (memtable_source != NULL)
			tp_source_foreach_document(
					memtable_source, tp_boolean_write_candidate, &writer);
	}
	else
	{
		hash_seq_init(&sequence, candidates);
		while ((candidate = hash_seq_search(&sequence)) != NULL)
			tp_boolean_write_candidate(candidate, &writer);
	}

	if (memtable_source != NULL)
		tp_source_close(memtable_source);

	for (int level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber segment = metap->level_heads[level];

		while (segment != InvalidBlockNumber)
		{
			TpSegmentReader *reader =
					tp_segment_open_ex(scan->indexRelation, segment, false);

			tp_boolean_write_segment(reader, &writer);
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
	MemoryContextSwitchTo(old_context);

	return writer.count > 0;
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
