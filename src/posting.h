/*
 * posting.h - Tapir In-Memory Posting Lists
 *
 * This module implements in-memory posting lists for Tapir indexes.  These
 * are used to buffer updates until the index is flushed to disk.
 */

#pragma once

#include "postgres.h"
#include "storage/itemptr.h"
#include "storage/lwlock.h"
#include "storage/spin.h"
#include "utils/hsearch.h"
#include "memtable.h"			/* For TpCorpusStatistics definition */
#include "stringtable.h"		/* For TpStringHashTable definition */

/* TpPostingEntry and TpPostingList are now defined in memtable.h */

/*
 * Hash table entry for term_id -> posting list mapping
 */
typedef struct TpPostingHashEntry
{
	int32		term_id;		/* Hash key */
	TpPostingList posting_list;	/* Hash table value (embedded) */
}			PostingHashEntry;

/*
 * Note: TpIndexState is now defined in memtable.h as a DSA-based structure
 * The old shared memory version has been removed
 */

/*
 * Global shared state for all Tapir indexes
 */
typedef struct PostingSharedState
{
	/* Inherited from memtable.h */
	LWLock	   *string_interning_lock;

	/* New posting list components */
	LWLock	   *index_registry_lock;	/* Protects index_registry */
	int32		num_indexes;	/* Number of active indexes */
	Size		total_memory_used;	/* Global memory usage */

	/* Index registry - maps index OID to TpIndexState */
	HTAB	   *index_registry;
}			PostingSharedState;

/* Configuration parameters */
extern int	tp_posting_list_growth_factor;	/* Array growth multiplier */

/* Core posting list operations */
extern void tp_posting_init_shared_state(void);
extern TpIndexState * tp_get_index_state(Oid index_oid,
											 const char *index_name);

/* Document and term management */
extern void tp_add_document_terms(TpIndexState * index_state,
									ItemPointer ctid,
									char **terms,
									int32 *frequencies,
									int term_count,
									int32 doc_length);
extern TpPostingList * tp_get_posting_list(TpIndexState * index_state,
											   const char *term);
extern TpPostingList * tp_get_or_create_posting_list(TpIndexState * index_state,
													 const char *term);
extern void tp_add_document_to_posting_list(TpIndexState *index_state,
											TpPostingList *posting_list,
											ItemPointer ctid,
											int32 frequency);

/* Document length hash table operations */
extern int32 tp_get_document_length(TpIndexState *index_state, ItemPointer ctid);

/* Index building operations */
extern void tp_finalize_index_build(TpIndexState * index_state);
extern void tp_calculate_average_idf(TpIndexState *index_state);
extern float4 tp_calculate_idf(int32 doc_freq, int32 total_docs, float4 average_idf);

/* Query operations */
extern float4 bm25_score_document(TpIndexState * index_state,
								  ItemPointer ctid,
								  char **query_terms,
								  int32 *query_frequencies,
								  int query_term_count,
								  float4 k1,
								  float4 b);

extern int	tp_score_documents(TpIndexState * index_state,
								   char **query_terms,
								   int32 *query_frequencies,
								   int query_term_count,
								   float4 k1,
								   float4 b,
								   int max_results,
								   ItemPointer result_ctids,
								   float4 **result_scores);

/* Statistics and maintenance */
extern TpCorpusStatistics * tp_get_corpus_statistics(TpIndexState * index_state);

/* Shared memory cleanup */
extern void tp_cleanup_index_shared_memory(Oid index_oid);

