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
	float4		frequency;		/* Term frequency in document */
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
	TpPostingEntry *entries;	/* Allocated separately */
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
 * Main shared memory state structure
 */
typedef struct TpSharedState
{
	LWLock	   *string_interning_lock;	/* Protects string_hash operations */
	Size		string_hash_size;	/* Current hash table size */
	int32		total_terms;	/* Total unique terms interned */

	/* Future: Add other memtable components here */
	LWLock	   *posting_lists_lock; /* Future: protects posting lists */
	TpCorpusStatistics stats;		/* Corpus statistics */
}			TpSharedState;

/* Global variables */
extern TpSharedState * tp_shared_state;
extern HTAB *tp_query_limits_hash; /* Per-backend hash table */

/* Query limit tracking for LIMIT optimization (per-backend) */
typedef struct TpQueryLimitEntry
{
	Oid			index_oid;		/* Index OID as key */
	int			limit;			/* LIMIT value from query */
}			TpQueryLimitEntry;

/* GUC variables */
extern int	tp_shared_memory_size;
extern int	tp_default_limit;

/* Function declarations */

/* Extension lifecycle */
extern void tp_init_memtable_hooks(void);
extern void tp_shmem_request(void);
extern void tp_shmem_startup(void);

/* Transaction-level lock management */
extern void tp_transaction_lock_acquire(void);
extern void tp_transaction_lock_release(void);
extern bool tp_transaction_lock_held;

/* String interning API */
extern void tp_intern_string(const char *term);
extern void tp_intern_string_len(const char *term, int term_len);

/* Statistics and maintenance */

/* Memory management */
extern Size tp_calculate_shared_memory_size(void);

#endif							/* TP_MEMTABLE_H */
