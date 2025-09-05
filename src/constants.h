#ifndef CONSTANTS_H
#define CONSTANTS_H

/* Extension name and version */
#define TP_EXTENSION_NAME "tapir"

/* Magic numbers and metadata */
#define TP_MAGIC 0x25424D25	/* "%BM%" in hex */
#define TP_VERSION 1
#define TP_METAPAGE_BLKNO 0

/* BM25 scoring constants */
#define TP_DEFAULT_K1 1.2
#define TP_DEFAULT_B 0.75

/* Memory and capacity limits */
#define TP_QUERY_LIMITS_HASH_SIZE 128
#define TP_DEFAULT_SHARED_MEMORY_SIZE 4	/* Default 4MB per index */
#define TP_MAX_SHARED_MEMORY_SIZE 1024		/* Max 1GB per index */
#define TP_DEFAULT_QUERY_LIMIT 1000		/* Default limit when none detected */
#define TP_MAX_QUERY_LIMIT 100000			/* Max 100k */
#define TP_DEFAULT_SEGMENT_THRESHOLD 10000

/* Memory budget distribution (per index) */
#define TP_AVG_TERM_LENGTH 5				/* Average term length in characters */
#define TP_STRING_TABLE_MEMORY_FRACTION 0.25	/* 25% of memory for string table */
#define TP_POSTING_LISTS_MEMORY_FRACTION 0.75	/* 75% of memory for posting lists */

/* Hash table sizes */
#define TP_STRING_INTERNING_HASH_SIZE 1024
#define TP_POSTING_LIST_HASH_INITIAL_SIZE 32
#define TP_POSTING_LIST_HASH_MAX_SIZE 256

/* Posting list and array parameters */
#define TP_INITIAL_POSTING_LIST_CAPACITY 16
#define TP_POSTING_LIST_GROWTH_FACTOR 2

/* Query processing timeouts and limits */
#define TP_MAX_INDEX_NAME_LENGTH 1024
#define TP_MAX_BLOCK_NUMBER 1000000

/* Cost estimation constants */
#define TP_DEFAULT_TUPLE_ESTIMATE 1000.0
#define TP_HIGH_STARTUP_COST 1000000.0
#define TP_INDEX_SCAN_COST_FACTOR 0.1		/* Make index scan very attractive */
#define TP_DEFAULT_INDEX_SELECTIVITY 0.1	/* Assume 10% selectivity for text searches */
#define TP_DEFAULT_INDEX_PAGES 1000.0

/* Memory validation constants */
#define TP_MIN_MEMORY_ADDRESS 0x1000
#define TP_MAX_MEMORY_ADDRESS 0x7fffffffffff

#endif /* CONSTANTS_H */
