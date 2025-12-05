/*-------------------------------------------------------------------------
 *
 * operator.h
 *	  BM25 scoring and query operations for Tapir
 *
 * This module contains the top-level scoring functions that coordinate
 * between memtable, stringtable, and posting list components.
 *
 * IDENTIFICATION
 *	  src/operator.h
 *
 *-------------------------------------------------------------------------
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

/*
 * CTID-based score cache
 *
 * When a query like "SELECT content <@> query AS score ... ORDER BY score"
 * is executed, the <@> operator is evaluated twice: once by the index scan
 * (for ORDER BY) and once by the projection (for SELECT). The CTID cache
 * avoids duplicate scoring by caching the score from the index scan.
 *
 * The cache is keyed by CTID + table OID + query hash. The table OID is
 * needed to distinguish CTIDs from different partitions (CTIDs are only
 * unique within a single table).
 */
extern void tp_cache_score(
		ItemPointer ctid, Oid table_oid, uint32 query_hash, float4 score);
extern float4 tp_get_cached_score(
		ItemPointer ctid, Oid table_oid, uint32 query_hash, bool *found);
extern uint32 tp_hash_query(Oid index_oid, const char *query_text);
extern void	  tp_clear_score_cache(void);

/*
 * Current row CTID and table OID tracking for score cache lookup.
 *
 * When the index scan returns a row via tp_gettuple(), we store the CTID
 * and table OID. When text_tpquery_score() is called (from SELECT list),
 * it uses these to look up the cached score.
 */
extern void		   tp_set_current_row_ctid(ItemPointer ctid);
extern ItemPointer tp_get_current_row_ctid(void);
extern void		   tp_set_current_row_table_oid(Oid table_oid);
extern Oid		   tp_get_current_row_table_oid(void);
extern void		   tp_set_current_query_hash(uint32 hash);
extern uint32	   tp_get_current_query_hash(bool *valid);
