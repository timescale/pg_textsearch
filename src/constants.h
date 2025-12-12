/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * constants.h - Shared constants and configuration
 */
#pragma once

/*
 * Page magic numbers - unique identifiers for each page type.
 * These are used to validate page contents and detect corruption.
 */
#define TP_METAPAGE_MAGIC	0x5450494D /* "TPIM" - Tapir Index Metapage */
#define TP_DOCID_PAGE_MAGIC 0x54504944 /* "TPID" - Tapir Docid Page */
#define TP_SEGMENT_MAGIC	0x54505347 /* "TPSG" - Tapir Segment */
#define TP_PAGE_INDEX_MAGIC 0x54505049 /* "TPPI" - Tapir Page Index */

/*
 * Page format versions - bump when on-disk format changes.
 * Each page type has its own version for independent evolution.
 */
#define TP_METAPAGE_VERSION                                                   \
	4							/* Bumped for consistent magic/version naming \
								 */
#define TP_DOCID_PAGE_VERSION 1 /* Initial version */
#define TP_SEGMENT_VERSION	  1 /* Segment format version */
#define TP_PAGE_INDEX_VERSION 1 /* Page index format version */

#define TP_METAPAGE_BLKNO 0

/* Segment hierarchy configuration */
#define TP_MAX_LEVELS				  8 /* Supports 8^8 = 16M segments */
#define TP_DEFAULT_SEGMENTS_PER_LEVEL 8

/* BM25 scoring constants */
#define TP_DEFAULT_K1 1.2
#define TP_DEFAULT_B  0.75

/* Memory and capacity limits */
#define TP_QUERY_LIMITS_HASH_SIZE	   128
#define TP_DEFAULT_QUERY_LIMIT		   1000
#define TP_MAX_QUERY_LIMIT			   100000
#define TP_DEFAULT_SEGMENT_THRESHOLD   10000
#define TP_DEFAULT_BULK_LOAD_THRESHOLD 100000 /* terms/xact trigger spill */
#define TP_DEFAULT_MEMTABLE_SPILL_THRESHOLD \
	800000 /* posting entries to trigger spill (~8MB segments) */

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

/*
 * Global GUC variables declared in mod.c
 * Note: tp_relopt_kind is declared in index.c as it requires
 * access/reloptions.h
 */
extern bool tp_log_scores;
extern int	tp_bulk_load_threshold;
extern int	tp_memtable_spill_threshold;
extern int	tp_segments_per_level;
