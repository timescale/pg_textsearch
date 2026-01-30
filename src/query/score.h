/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * score.h - BM25 scoring operator interface
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

/* GUC variables for BMW configuration - defined in mod.c */
extern bool tp_log_bmw_stats;
extern bool tp_enable_bmw;

/*
 * Initialize HASHCTL for document score hash tables.
 * Uses ItemPointerData as key (CTID).
 */
static inline void
tp_init_doc_hash_ctl(HASHCTL *ctl, Size entry_size, MemoryContext memctx)
{
	memset(ctl, 0, sizeof(HASHCTL));
	ctl->keysize   = sizeof(ItemPointerData);
	ctl->entrysize = entry_size;
	if (memctx != NULL)
		ctl->hcxt = memctx;
}
