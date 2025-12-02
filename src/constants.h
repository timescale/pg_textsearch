#pragma once

/* Magic numbers and metadata */
#define TP_MAGIC		  0x3A7F9B2E
#define TP_VERSION		  2 /* Bumped for first_segment field in metapage */
#define TP_METAPAGE_BLKNO 0

/* BM25 scoring constants */
#define TP_DEFAULT_K1 1.2
#define TP_DEFAULT_B  0.75

/* Memory and capacity limits (not all currently enforced) */
#define TP_QUERY_LIMITS_HASH_SIZE	  128
#define TP_DEFAULT_INDEX_MEMORY_LIMIT 64
#define TP_MAX_INDEX_MEMORY_LIMIT	  512
#define TP_DEFAULT_QUERY_LIMIT		  1000
#define TP_MAX_QUERY_LIMIT			  100000
#define TP_DEFAULT_SEGMENT_THRESHOLD  10000
#define TP_DEFAULT_BULK_LOAD_THRESHOLD \
	100000 /* terms per xact to trigger spill */

/* Hash table sizes */
#define TP_STRING_INTERNING_HASH_SIZE	  1024
#define TP_POSTING_LIST_HASH_INITIAL_SIZE 32
#define TP_POSTING_LIST_HASH_MAX_SIZE	  256

/* Posting list and array parameters */
#define TP_INITIAL_POSTING_LIST_CAPACITY 16
#define TP_POSTING_LIST_GROWTH_FACTOR	 2

/* Query processing timeouts and limits */
#define TP_MAX_INDEX_NAME_LENGTH 1024
#define TP_MAX_BLOCK_NUMBER		 1000000

/* Cost estimation constants */
#define TP_DEFAULT_TUPLE_ESTIMATE	 1000.0
#define TP_HIGH_STARTUP_COST		 1000000.0
#define TP_INDEX_SCAN_COST_FACTOR	 0.1
#define TP_DEFAULT_INDEX_SELECTIVITY 0.1
#define TP_DEFAULT_INDEX_PAGES		 1000.0

/*
 * Fixed LWLock tranche IDs.
 * These must be consistent across all backends to allow DSA attachment.
 * We use high numbers to avoid conflicts with core PostgreSQL tranches.
 */
#define TP_TRANCHE_STRING	   1001
#define TP_TRANCHE_POSTING	   1002
#define TP_TRANCHE_CORPUS	   1003
#define TP_TRANCHE_DOC_LENGTHS 1004
