/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * query/score.h - BM25 scoring operator interface
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>

typedef struct TpLocalIndexState TpLocalIndexState;

/*
 * Document score entry for query result accumulation.
 * Used by both segment/scan.c and types/score.c for BM25 scoring.
 */
typedef struct DocumentScoreEntry
{
	ItemPointerData ctid;
	float4			score;
	float4			doc_length;
} DocumentScoreEntry;

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

/*
 * Calculate BM25 term score for a single term in a document.
 * Formula: IDF * tf*(k1+1) / (tf + k1*(1-b+b*dl/avgdl)) * query_freq
 */
static inline float4
tp_calculate_bm25_term_score(
		float4 tf,
		float4 idf,
		float4 doc_length,
		float4 avg_doc_len,
		float4 k1,
		float4 b,
		float4 query_frequency)
{
	double numerator   = (double)tf * ((double)k1 + 1.0);
	double denominator = (double)tf +
						 (double)k1 * (1.0 - (double)b +
									   (double)b * ((double)doc_length /
													(double)avg_doc_len));
	return (float4)((double)idf * (numerator / denominator) *
					(double)query_frequency);
}
