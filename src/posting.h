/*
 * posting.h - Tp In-Memory Posting Lists
 *
 * This module implements in-memory posting lists for Tp indexes.  These
 * are used to buffer updates until the index is flushed to disk.
 */

#ifndef POSTING_H
#define POSTING_H

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
 * Per-index shared memory state
 * Each Tp index gets its own instance in shared memory
 */
typedef struct TpIndexState
{
	Oid			index_oid;		/* PostgreSQL index OID */
	char		index_name[NAMEDATALEN];	/* Index relation name */

	/* Note: Posting lists are now stored directly in string interning table */

	/* Locking and synchronization */
	LWLock	   *build_lock;		/* Exclusive for index building */
	LWLock	   *stats_lock;		/* Protects statistics updates */
	slock_t		doc_id_mutex;	/* Protects next_doc_id counter */

	/* Index metadata */
				TpCorpusStatistics
				stats;			/* Corpus statistics (k1/b in metapage) */
	int32		next_doc_id;	/* Atomic counter for document IDs */
	Oid			text_config_oid;	/* Text search configuration */
	bool		is_finalized;	/* Whether index building is complete */

	/* Memory management */
	Size		memory_used;	/* Current memory usage estimate */
	Size		memory_limit;	/* Maximum memory usage limit */
	uint32		total_posting_entries;	/* Total posting entries across all terms */
	uint32		max_posting_entries;	/* Maximum posting entries allowed */
	
	/* String table reference - for v0.0a we use the shared table */
	TpStringHashTable *string_table;	/* Points to shared string table */
	
	/* Recovery state for memtable-only implementation */
	bool		needs_rebuild;	/* Posting lists need to be rebuilt from disk */
	BlockNumber	first_docid_page;	/* First page of docid chain for recovery */
}			TpIndexState;

/*
 * Global shared state for all Tp indexes
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
									float4 *frequencies,
									int term_count,
									int32 doc_length);
extern TpPostingList * tp_get_posting_list(TpIndexState * index_state,
											   const char *term);
extern TpPostingList * tp_get_or_create_posting_list(TpIndexState * index_state,
													 const char *term);

/* Index building operations */
extern void tp_finalize_index_build(TpIndexState * index_state);

/* Query operations */
extern float4 bm25_score_document(TpIndexState * index_state,
								  ItemPointer ctid,
								  char **query_terms,
								  float4 *query_frequencies,
								  int query_term_count,
								  float4 k1,
								  float4 b);

extern int	tp_score_documents(TpIndexState * index_state,
								   char **query_terms,
								   float4 *query_frequencies,
								   int query_term_count,
								   float4 k1,
								   float4 b,
								   int max_results,
								   ItemPointer result_ctids,
								   float4 **result_scores);

/* Statistics and maintenance */
extern TpCorpusStatistics * tp_get_corpus_statistics(TpIndexState * index_state);

#endif							/* POSTING_H */
