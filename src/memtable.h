/*-------------------------------------------------------------------------
 *
 * memtable.h
 *	  Tapir in-memory table structures and function declarations
 *
 * IDENTIFICATION
 *	  src/memtable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TP_MEMTABLE_H
#define TP_MEMTABLE_H

#include "postgres.h"
#include "constants.h"
#include "storage/itemptr.h"
#include "storage/lwlock.h"
#include "storage/spin.h"
#include "utils/hsearch.h"
#include "utils/timestamp.h"
#include "utils/dsa.h"
#include "storage/dsm_registry.h"

/*
 * Forward declarations to avoid circular dependencies
 */
typedef struct TpPostingEntry TpPostingEntry;
typedef struct TpPostingList TpPostingList;

/*
 * Individual document occurrence within a posting list
 */
struct TpPostingEntry
{
	ItemPointerData ctid;		/* Document heap tuple ID */
	int32		doc_id;			/* Internal document ID */
	int32		frequency;		/* Term frequency in document */
	int32		doc_length;		/* Document length */
};

/*
 * Posting list for a single term
 * Uses dynamic arrays with O(1) amortized inserts during building,
 * then sorts once at finalization for optimal query performance
 */
struct TpPostingList
{
	int32		doc_count;		/* Number of documents containing term */
	int32		capacity;		/* Allocated array capacity */
	bool		is_finalized;	/* True after final sort for queries */
	int32		doc_freq;		/* Document frequency (for IDF calculation) */

	/* Dynamic array - unsorted during building, sorted after finalization */
	dsa_pointer	entries_dp;		/* DSA pointer to TpPostingEntry array */
};


/*
 * BM25 algorithm parameters and corpus statistics
 */
typedef struct TpCorpusStatistics
{
	/* Corpus-level statistics */
	int32		total_docs;		/* Total number of documents */
	int64		total_terms;	/* Total term occurrences across corpus */
	int64		total_len;		/* Total length of all documents */
	float4		max_doc_length; /* Maximum document length */
	float4		min_doc_length; /* Minimum document length */

	/* BM25 algorithm parameters */
	float4		k1;				/* Term frequency saturation parameter */
	float4		b;				/* Document length normalization factor */

	/* Performance monitoring */
	uint64		queries_executed;	/* Total queries processed */
	uint64		terms_interned; /* Total strings interned */
	uint64		cache_hits;		/* String interning cache hits */
	uint64		cache_misses;	/* String interning cache misses */

	/* Maintenance metadata */
	TimestampTz last_checkpoint;	/* Last disk checkpoint time */
	int32		segment_threshold;	/* Threshold for flushing to disk segments */
}			TpCorpusStatistics;

/*
 * Per-index state structure stored in DSA
 * Each index gets its own DSA area and this structure is the root
 */
typedef struct TpIndexState
{
	/* Per-index locks allocated within this DSA */
	LWLock		string_interning_lock;	/* Protects string hash operations */
	LWLock		posting_lists_lock;		/* Protects posting list operations */
	LWLock		corpus_stats_lock;		/* Protects corpus statistics */

	/* String hash table stored in this DSA */
	dsa_pointer	string_hash_dp;			/* DSA pointer to TpStringHashTable */
	int32		total_terms;			/* Total unique terms interned */

	/* Corpus statistics for BM25 */
	TpCorpusStatistics stats;			/* Corpus statistics */

	/* DSA management */
	Oid			index_oid;				/* Index OID for this state */
	int			string_tranche_id;		/* Tranche ID for string interning lock */
	int			posting_tranche_id;		/* Tranche ID for posting lists lock */  
	int			corpus_tranche_id;		/* Tranche ID for corpus stats lock */
	dsa_handle	area_handle;			/* DSA handle for cross-process attachment */
	bool		dsa_initialized;		/* True when DSA area is ready for use */
}			TpIndexState;

/* Per-backend hash table for query limits (unchanged) */
extern HTAB *tp_query_limits_hash;

/* Query limit tracking for LIMIT optimization (per-backend) */
typedef struct TpQueryLimitEntry
{
	Oid			index_oid;		/* Index OID as key */
	int			limit;			/* LIMIT value from query */
}			TpQueryLimitEntry;

/* GUC variables */
extern int	tp_index_memory_limit;	/* Currently not enforced */
extern int	tp_default_limit;

/* Default hash table size */
#define TP_DEFAULT_HASH_BUCKETS		1024

/* Cache entry for index states */
typedef struct IndexStateCacheEntry
{
	Oid			index_oid;			/* Hash key: index OID */
	TpIndexState *state;			/* Cached index state pointer */
	dsa_area   *area;				/* Cached DSA area pointer */
} IndexStateCacheEntry;

/* Function declarations */

/* DSA-based per-index management */
extern char *tp_get_dsm_segment_name(Oid index_oid);
extern TpIndexState *tp_get_or_create_index_dsa(Oid index_oid, dsa_area **area_out);
extern void tp_destroy_index_dsa(Oid index_oid);
extern dsa_area *tp_get_dsa_area_for_index(TpIndexState *index_state, Oid index_oid);
extern IndexStateCacheEntry *get_cached_index_state(Oid index_oid);

/* DSA memory allocation wrappers */
extern dsa_pointer tp_dsa_allocate(dsa_area *area, Size size);
extern void tp_dsa_free(dsa_area *area, dsa_pointer dp);
extern void *tp_dsa_get_address(dsa_area *area, dsa_pointer dp);

/* Transaction-level lock management */
extern void tp_transaction_lock_acquire(void);
extern void tp_transaction_lock_release(void);
extern bool tp_transaction_lock_held;

/* String interning API */
extern void tp_intern_string(const char *term);
extern void tp_intern_string_len(const char *term, int term_len);

/* Statistics and maintenance */

#endif							/* TP_MEMTABLE_H */
