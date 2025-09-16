/*
 * posting.h - Tapir In-Memory Posting Lists
 *
 * This module implements in-memory posting lists for Tapir indexes.  These
 * are used to buffer updates until the index is flushed to disk.
 */

#pragma once

#include <postgres.h>

#include <storage/itemptr.h>
#include <storage/lwlock.h>
#include <storage/spin.h>
#include <utils/hsearch.h>

#include "memtable.h"
#include "stringtable.h"

/* Array growth multiplier */
extern int tp_posting_list_growth_factor;

/* Document and term management */
extern void tp_add_document_terms(
		TpIndexState *index_state,
		ItemPointer	  ctid,
		char		**terms,
		int32		 *frequencies,
		int			  term_count,
		int32		  doc_length);

extern TpPostingList *
tp_get_posting_list(TpIndexState *index_state, const char *term);

extern TpPostingList *
tp_get_or_create_posting_list(TpIndexState *index_state, const char *term);

extern void tp_add_document_to_posting_list(
		TpIndexState  *index_state,
		TpPostingList *posting_list,
		ItemPointer	   ctid,
		int32		   frequency);

/* Document length hash table operations */
extern int32
tp_get_document_length(TpIndexState *index_state, ItemPointer ctid);

/* Index building operations */
extern void tp_finalize_index_build(TpIndexState *index_state);
extern void tp_calculate_average_idf(TpIndexState *index_state);
extern float4
tp_calculate_idf(int32 doc_freq, int32 total_docs, float4 average_idf);

/* Query operations */
extern float4 bm25_score_document(
		TpIndexState *index_state,
		ItemPointer	  ctid,
		char		**query_terms,
		int32		 *query_frequencies,
		int			  query_term_count,
		float4		  k1,
		float4		  b);

extern int tp_score_documents(
		TpIndexState *index_state,
		Relation	  index_relation,
		char		**query_terms,
		int32		 *query_frequencies,
		int			  query_term_count,
		float4		  k1,
		float4		  b,
		int			  max_results,
		ItemPointer	  result_ctids,
		float4		**result_scores);

/* Statistics and maintenance */
extern TpCorpusStatistics *tp_get_corpus_statistics(TpIndexState *index_state);

/* Shared memory cleanup */
extern void tp_cleanup_index_shared_memory(Oid index_oid);
