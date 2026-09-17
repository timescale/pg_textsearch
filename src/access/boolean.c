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
#include <utils/memutils.h>

#include "access/am.h"
#include "access/boolean.h"
#include "memtable/chain_walker.h"
#include "segment/alive_bitset.h"
#include "segment/io.h"
#include "types/vector.h"

#define TP_BOOLEAN_MAX_PREFIX_ITERATORS 256

typedef struct TpBooleanTerm
{
	char *lexeme;
	int	  length;
} TpBooleanTerm;

typedef struct TpBooleanEvalState
{
	TSQuery		   query;
	TpBooleanTerm *terms;
	int			   term_count;
	int			   exact_operand_count;
	bool		   requires_recheck;
} TpBooleanEvalState;

typedef enum TpBooleanCandidateKind
{
	TP_BOOLEAN_CANDIDATE_ALL,
	TP_BOOLEAN_CANDIDATE_TERM,
	TP_BOOLEAN_CANDIDATE_UNION,
	TP_BOOLEAN_CANDIDATE_PREFIX,
	TP_BOOLEAN_CANDIDATE_PREFIX_BITMAP,
} TpBooleanCandidateKind;

typedef struct TpBooleanPrefixHeapNode
{
	uint32 iterator_index;
	uint32 doc_id;
} TpBooleanPrefixHeapNode;

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

		struct
		{
			TpSegmentPostingIterator *iterators;
			TpBooleanPrefixHeapNode	 *heap;
			uint32					  iterator_count;
			uint32					  heap_size;
			bool					  initialized;
		} prefix;

		struct
		{
			uint8 *bits;
			uint32 next_doc_id;
			uint32 num_docs;
		} prefix_bitmap;
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

typedef struct TpBooleanSegmentSnapshot
{
	BlockNumber *roots;
	uint32		 count;
	uint32		 capacity;
} TpBooleanSegmentSnapshot;

static List *tp_boolean_incomplete_warning_seen = NIL;

static void
tp_boolean_warn_if_incomplete(
		Relation index, TpIndexMetaPage metap, TSQuery query)
{
	Oid index_oid = RelationGetRelid(index);

	if ((metap->capabilities & TP_METAPAGE_ALL_DOCUMENTS_INDEXED) != 0 ||
		query->size == 0 || tsquery_requires_match(GETQUERY(query)) ||
		list_member_oid(tp_boolean_incomplete_warning_seen, index_oid))
		return;

	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		tp_boolean_incomplete_warning_seen =
				lappend_oid(tp_boolean_incomplete_warning_seen, index_oid);
		MemoryContextSwitchTo(old);
	}

	ereport(WARNING,
			(errmsg("BM25 index \"%s\" may omit empty or stopword-only "
					"documents from this query",
					RelationGetRelationName(index)),
			 errhint("Run \"REINDEX INDEX %s;\" for complete results.",
					 RelationGetRelationName(index))));
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
	state.exact_operand_count = tp_boolean_query_exact_operand_count(query);

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
			 * Segment postings do not retain lexeme weights, and prefix
			 * operands may expand to many terms. Treat these operands as
			 * unknown and let the heap recheck evaluate their exact
			 * semantics.
			 */
			if (operand->prefix || operand->weight != 0)
			{
				state.requires_recheck = true;
				continue;
			}

			if (tp_boolean_find_term(&state, lexeme, operand->length) != NULL)
				continue;

			term		 = &state.terms[state.term_count++];
			term->length = operand->length;
			term->lexeme = pnstrdup(lexeme, operand->length);
		}
	}

	return state;
}

int
tp_boolean_query_exact_operand_count(TSQuery query)
{
	QueryItem *items = GETQUERY(query);
	int		   count = 0;

	for (int i = 0; i < query->size; i++)
	{
		if (items[i].type == QI_VAL && !items[i].qoperand.prefix &&
			items[i].qoperand.weight == 0)
			count++;
	}

	return count;
}

static TpBooleanSegmentSnapshot
tp_boolean_segment_snapshot_create(Relation index, const TpIndexMetaPage metap)
{
	TpBooleanSegmentSnapshot snapshot = {0};
	BlockNumber				 nblocks  = RelationGetNumberOfBlocks(index);

	for (int level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber segment = metap->level_heads[level];

		while (segment != InvalidBlockNumber)
		{
			if (snapshot.count >= nblocks)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("BM25 segment chain contains a cycle")));
			if (snapshot.count == snapshot.capacity)
			{
				snapshot.capacity = snapshot.capacity == 0
										  ? 16
										  : snapshot.capacity * 2;
				snapshot.roots =
						snapshot.roots == NULL
								? palloc_array(BlockNumber, snapshot.capacity)
								: repalloc(
										  snapshot.roots,
										  snapshot.capacity *
												  sizeof(BlockNumber));
			}

			snapshot.roots[snapshot.count++] = segment;
			if (!tp_segment_read_next(index, segment, &segment))
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("could not open BM25 segment %u", segment)));
		}
	}

	return snapshot;
}

typedef struct TpBooleanResultWriter
{
	TpBooleanEvalState *query;
	BufFile			   *file;
	int					count;
} TpBooleanResultWriter;

typedef struct TpBooleanMemtableEval
{
	TpBooleanEvalState *query;
	bool			   *term_present;
} TpBooleanMemtableEval;

static void
tp_boolean_write_result(TpBooleanResultWriter *writer, ItemPointer candidate)
{
	BufFileWrite(writer->file, candidate, sizeof(ItemPointerData));
	writer->count++;
}

static TSTernaryValue
tp_boolean_memtable_has_term(
		void *arg, QueryOperand *operand, ExecPhraseData *data)
{
	TpBooleanMemtableEval *eval = arg;
	const char	  *lexeme = GETOPERAND(eval->query->query) + operand->distance;
	TpBooleanTerm *term =
			tp_boolean_find_term(eval->query, lexeme, operand->length);

	if (operand->prefix || operand->weight != 0)
		return TS_MAYBE;

	Assert(term != NULL);
	if (!eval->term_present[term - eval->query->terms])
		return TS_NO;
	return data == NULL ? TS_YES : TS_MAYBE;
}

static void
tp_boolean_write_memtable_snapshot(
		Relation					   index,
		const TpMemtableChainSnapshot *snapshot,
		TpBooleanResultWriter		  *writer)
{
	TpBooleanEvalState	 *state = writer->query;
	TpBooleanMemtableEval eval	= {
			 .query = state,
	 };
	TpChainWalker	   *walker;
	TpChainWalkerRecord rec;

	if (!BlockNumberIsValid(snapshot->head_blkno))
		return;

	eval.term_present = palloc0(Max(state->term_count, 1) * sizeof(bool));
	walker			  = tp_chain_walker_open_bounded(
			   index, snapshot, CurrentMemoryContext);

	while (tp_chain_walker_next(walker, &rec))
	{
		TpVector	  *vec = (TpVector *)rec.vector_bytes;
		TpVectorEntry *entry;

		CHECK_FOR_INTERRUPTS();
		memset(eval.term_present, 0, state->term_count * sizeof(bool));

#ifdef USE_ASSERT_CHECKING
		if (!rec.is_fragment)
			tpvector_validate_v2_buffer(rec.vector_bytes, rec.vector_len);
#endif

		entry = get_tpvector_first_entry(vec);
		for (int i = 0; i < vec->entry_count; i++)
		{
			TpVectorEntryView view;
			TpBooleanTerm	 *term;

			entry = tpvector_entry_decode_advance(entry, &view);
			term  = tp_boolean_find_term(
					 state, view.lexeme, (int)view.lexeme_len);
			if (term != NULL)
				eval.term_present[term - state->terms] = true;
		}

		if (TS_execute_ternary(
					GETQUERY(state->query),
					&eval,
					TS_EXEC_PHRASE_NO_POS,
					tp_boolean_memtable_has_term) != TS_NO)
			tp_boolean_write_result(writer, &rec.ctid);

		if (rec.owns_vector)
			pfree((void *)rec.vector_bytes);
	}

	tp_chain_walker_close(walker);
	pfree(eval.term_present);
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
tp_boolean_create_union_stream(
		TpSegmentReader			 *reader,
		TpBooleanCandidateStream *left,
		TpBooleanCandidateStream *right)
{
	TpBooleanCandidateStream *stream = palloc0(sizeof(*stream));

	stream->kind = TP_BOOLEAN_CANDIDATE_UNION;
	stream->estimate =
			Min((uint64)reader->header->num_docs,
				left->estimate + right->estimate);
	stream->state.union_stream.left	 = left;
	stream->state.union_stream.right = right;
	return stream;
}

static bool
tp_boolean_prefix_heap_less(
		const TpBooleanPrefixHeapNode *left,
		const TpBooleanPrefixHeapNode *right)
{
	return left->doc_id < right->doc_id ||
		   (left->doc_id == right->doc_id &&
			left->iterator_index < right->iterator_index);
}

static void
tp_boolean_prefix_heap_sift_down(
		TpBooleanPrefixHeapNode *heap, uint32 heap_size, uint32 parent)
{
	TpBooleanPrefixHeapNode node = heap[parent];

	while (parent < heap_size / 2)
	{
		uint32 child = parent * 2 + 1;

		if (child + 1 < heap_size &&
			tp_boolean_prefix_heap_less(&heap[child + 1], &heap[child]))
			child++;
		if (!tp_boolean_prefix_heap_less(&heap[child], &node))
			break;
		heap[parent] = heap[child];
		parent		 = child;
	}

	heap[parent] = node;
}

static void
tp_boolean_prefix_stream_init(TpBooleanCandidateStream *stream)
{
	for (uint32 i = 0; i < stream->state.prefix.iterator_count; i++)
	{
		TpSegmentPosting *posting;

		if (tp_segment_posting_iterator_next(
					&stream->state.prefix.iterators[i], &posting))
			stream->state.prefix.heap[stream->state.prefix.heap_size++] =
					(TpBooleanPrefixHeapNode){
							.iterator_index = i,
							.doc_id			= posting->doc_id,
					};
	}

	for (uint32 i = stream->state.prefix.heap_size / 2; i > 0; i--)
		tp_boolean_prefix_heap_sift_down(
				stream->state.prefix.heap,
				stream->state.prefix.heap_size,
				i - 1);

	stream->state.prefix.initialized = true;
}

static void
tp_boolean_prefix_stream_advance_root(TpBooleanCandidateStream *stream)
{
	TpBooleanPrefixHeapNode *heap = stream->state.prefix.heap;
	TpSegmentPosting		*posting;
	uint32					 iterator_index = heap[0].iterator_index;

	if (tp_segment_posting_iterator_next(
				&stream->state.prefix.iterators[iterator_index], &posting))
		heap[0].doc_id = posting->doc_id;
	else
		heap[0] = heap[--stream->state.prefix.heap_size];

	if (stream->state.prefix.heap_size > 0)
		tp_boolean_prefix_heap_sift_down(
				heap, stream->state.prefix.heap_size, 0);
}

static TpBooleanCandidateStream *
tp_boolean_create_prefix_stream(
		TpSegmentReader *reader, const char *prefix, int prefix_length)
{
	TpSegmentPrefixCandidates candidates;
	TpBooleanCandidateStream *stream = palloc0(sizeof(*stream));

	tp_segment_prefix_candidates_init(
			reader,
			prefix,
			prefix_length,
			TP_BOOLEAN_MAX_PREFIX_ITERATORS,
			&candidates);
	stream->estimate = candidates.estimate;

	if (candidates.doc_bitmap != NULL)
	{
		stream->kind					 = TP_BOOLEAN_CANDIDATE_PREFIX_BITMAP;
		stream->state.prefix_bitmap.bits = candidates.doc_bitmap;
		stream->state.prefix_bitmap.num_docs = reader->header->num_docs;
		return stream;
	}

	if (candidates.iterator_count == 0)
	{
		stream->kind = TP_BOOLEAN_CANDIDATE_TERM;
		return stream;
	}

	stream->kind						= TP_BOOLEAN_CANDIDATE_PREFIX;
	stream->estimate					= candidates.estimate;
	stream->state.prefix.iterators		= candidates.iterators;
	stream->state.prefix.iterator_count = candidates.iterator_count;
	stream->state.prefix.heap =
			palloc_array(TpBooleanPrefixHeapNode, candidates.iterator_count);
	return stream;
}

static TpBooleanCandidateStream *
tp_boolean_create_candidate_stream(
		TpBooleanEvalState *state, TpSegmentReader *reader, QueryItem *item)
{
	TpBooleanCandidateStream *stream;

	if (item->type == QI_VAL)
	{
		QueryOperand *operand = &item->qoperand;
		const char	 *lexeme  = GETOPERAND(state->query) + operand->distance;

		if (operand->prefix)
			return tp_boolean_create_prefix_stream(
					reader, lexeme, operand->length);

		if (operand->weight != 0)
			return tp_boolean_create_all_stream(reader);

		stream						   = palloc0(sizeof(*stream));
		stream->kind				   = TP_BOOLEAN_CANDIDATE_TERM;
		stream->state.term.initialized = tp_segment_posting_iterator_init(
				&stream->state.term.iterator, reader, lexeme);
		stream->state.term.iterator.force_copy = true;
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

		return tp_boolean_create_union_stream(reader, left, right);
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

	case TP_BOOLEAN_CANDIDATE_PREFIX:
		if (!stream->state.prefix.initialized)
			tp_boolean_prefix_stream_init(stream);
		if (stream->state.prefix.heap_size == 0)
			return false;

		*doc_id = stream->state.prefix.heap[0].doc_id;
		do
		{
			tp_boolean_prefix_stream_advance_root(stream);
		} while (stream->state.prefix.heap_size > 0 &&
				 stream->state.prefix.heap[0].doc_id == *doc_id);
		return true;

	case TP_BOOLEAN_CANDIDATE_PREFIX_BITMAP:
		while (stream->state.prefix_bitmap.next_doc_id <
			   stream->state.prefix_bitmap.num_docs)
		{
			uint32 candidate = stream->state.prefix_bitmap.next_doc_id;
			uint8  byte = stream->state.prefix_bitmap.bits[candidate >> 3];

			if ((candidate & 7) == 0 && byte == 0)
			{
				stream->state.prefix_bitmap.next_doc_id += 8;
				continue;
			}

			stream->state.prefix_bitmap.next_doc_id++;
			if (byte & (1 << (candidate & 7)))
			{
				*doc_id = candidate;
				return true;
			}
		}
		return false;
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
	else if (stream->kind == TP_BOOLEAN_CANDIDATE_PREFIX)
	{
		for (uint32 i = 0; i < stream->state.prefix.iterator_count; i++)
			tp_segment_posting_iterator_free(
					&stream->state.prefix.iterators[i]);
		pfree(stream->state.prefix.iterators);
		pfree(stream->state.prefix.heap);
	}
	else if (stream->kind == TP_BOOLEAN_CANDIDATE_PREFIX_BITMAP)
		pfree(stream->state.prefix_bitmap.bits);

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
	{
		cursors[i].initialized = tp_segment_posting_iterator_init(
				&cursors[i].iterator, reader, writer->query->terms[i].lexeme);
		cursors[i].iterator.force_copy = true;
	}

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

	so->boolean_query	= NULL;
	so->is_boolean_scan = true;
	so->boolean_recheck = false;

	if (keys[0].sk_flags & SK_ISNULL)
		return;

	tp_boolean_check_config(scan->indexRelation, metap);

	old_context		  = MemoryContextSwitchTo(so->scan_context);
	so->boolean_query = DatumGetTSQueryCopy(keys[0].sk_argument);
	MemoryContextSwitchTo(old_context);

	tp_boolean_warn_if_incomplete(
			scan->indexRelation, metap, so->boolean_query);
}

bool
tp_boolean_execute(IndexScanDesc scan, TpLocalIndexState *index_state)
{
	TpScanOpaque			so = (TpScanOpaque)scan->opaque;
	MemoryContext			old_context;
	TpBooleanEvalState		state;
	TpIndexMetaPage			metap;
	TpMemtableChainSnapshot memtable_snapshot = {
			.head_blkno = InvalidBlockNumber,
			.tail_blkno = InvalidBlockNumber,
	};
	TpBooleanSegmentSnapshot segments;
	TpBooleanResultWriter	 writer;

	if (so->boolean_query == NULL || so->boolean_query->size == 0)
		return false;

	if (index_state->lock_held)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BM25 Boolean execution must acquire its own index "
						"lock")));

	old_context = MemoryContextSwitchTo(so->boolean_context);
	state		= tp_boolean_extract_terms(so->boolean_query);
	if (state.exact_operand_count > TP_BOOLEAN_MAX_EXACT_OPERANDS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("BM25 Boolean queries support at most %d exact term "
						"operands",
						TP_BOOLEAN_MAX_EXACT_OPERANDS),
				 errdetail(
						 "Query contains %d exact term operands.",
						 state.exact_operand_count)));
	so->boolean_recheck = state.requires_recheck;

	tp_acquire_index_lock(index_state, LW_SHARED);
	metap = tp_get_metapage(scan->indexRelation);
	tp_boolean_check_config(scan->indexRelation, metap);
	tp_memtable_chain_snapshot_capture(
			scan->indexRelation,
			metap->memtable_head_blkno,
			metap->memtable_tail_blkno,
			&memtable_snapshot);
	segments = tp_boolean_segment_snapshot_create(scan->indexRelation, metap);

	pfree(metap);

	/*
	 * Spill or compaction can replace the captured chain and segment roots
	 * after the lock is released. Retired pages carry the publishing
	 * transaction's horizon, so VACUUM cannot recycle them while this scan's
	 * transaction snapshot can still reference them.
	 */
	tp_release_index_lock(index_state);

	writer.query = &state;
	writer.file	 = BufFileCreateTemp(false);
	writer.count = 0;

	tp_boolean_write_memtable_snapshot(
			scan->indexRelation, &memtable_snapshot, &writer);

	for (uint32 i = 0; i < segments.count; i++)
	{
		TpSegmentReader *reader = tp_segment_open_ex(
				scan->indexRelation, segments.roots[i], false);

		if (reader == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("could not open BM25 segment %u",
							segments.roots[i])));
		tp_boolean_write_segment(reader, &writer);
		tp_segment_close(reader);
	}
	if (segments.roots != NULL)
		pfree(segments.roots);

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
