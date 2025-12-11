/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * operator.h - BM25 scoring operator interface
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>

typedef struct TpLocalIndexState TpLocalIndexState;

extern int tp_score_documents(
		TpLocalIndexState *local_state,
		Relation		   index_relation,
		char			 **query_terms,
		int32			  *query_frequencies,
		int				   query_term_count,
		float4			   k1,
		float4			   b,
		int				   max_results,
		ItemPointer		   result_ctids,
		float4			 **result_scores);

/* IDF calculation */
extern float4 tp_calculate_idf(int32 doc_freq, int32 total_docs);
