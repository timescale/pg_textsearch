/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
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
#define TP_SEGMENT_MAGIC	0x54505347 /* "TPSG" - Tapir Segment */
#define TP_PAGE_INDEX_MAGIC 0x54505049 /* "TPPI" - Tapir Page Index */
#define TP_MEMTABLE_PAGE_MAGIC                                              \
	0x5450544D /* "TPTM" - Tapir Memtable (replaces docid pages; introduced \
				* in the memtable v2 redesign, see issue #374) */

/*
 * Page format versions - bump when on-disk format changes.
 * Each page type has its own version for independent evolution.
 */
/*
 * v7: memtable v2 redesign (issue #374).  Adds memtable_head_blkno
 * and memtable_tail_blkno at the end of TpIndexMetaPageData.  We
 * read v6 metapages compatibly: a v6 page zero-fills those slots,
 * so the read path explicitly maps 0 → InvalidBlockNumber for the
 * two new fields.  An empty v6 metapage is therefore equivalent to
 * a v7 metapage with an empty chain.  The first write
 * (memtable_bootstrap_and_append or tp_spill_finalize) lazily bumps
 * the on-disk version to 7 under the same GenericXLog that
 * initializes the chain head/tail or resets them.
 */
#define TP_METAPAGE_VERSION	   7
#define TP_METAPAGE_VERSION_V6 6 /* compat: read-only, lazy-upgrade */
#define TP_PAGE_INDEX_VERSION  1 /* Page index format version */
/* Initial version (memtable v2 redesign) */
#define TP_MEMTABLE_PAGE_VERSION 1

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
	32000000 /* posting entries to trigger spill (~1M docs/segment) */

/*
 * Lower bound on postings for VACUUM-cleanup and shutdown-hook
 * spills.  Below this, leaving the documents on the in-relation
 * memtable chain is cheaper than writing a runt L0 segment — LSM
 * compaction would spend more work consolidating tiny segments
 * than the chain pages cost to scan during reads.
 */
#define TP_MIN_SPILL_POSTINGS 1000

/* Hash table sizes */
#define TP_STRING_INTERNING_HASH_SIZE	  1024
#define TP_POSTING_LIST_HASH_INITIAL_SIZE 32
#define TP_POSTING_LIST_HASH_MAX_SIZE	  256

/* Posting list and array parameters */
#define TP_INITIAL_POSTING_LIST_CAPACITY 16
#define TP_POSTING_LIST_GROWTH_FACTOR	 2

/* Query processing timeouts and limits */
#define TP_MAX_INDEX_NAME_LENGTH 1024
#define TP_MAX_TERM_LENGTH		 (1024 * 1024) /* 1MB sanity limit */

/* Cost estimation constants */
#define TP_DEFAULT_TUPLE_ESTIMATE	 1000.0
#define TP_HIGH_STARTUP_COST		 1000000.0
#define TP_INDEX_SCAN_COST_FACTOR	 0.1
#define TP_DEFAULT_INDEX_SELECTIVITY 0.1
#define TP_DEFAULT_INDEX_PAGES		 1000.0

/*
 * Fixed LWLock tranche IDs.
 * These must be consistent across all backends to allow DSA attachment.
 * We use high numbers (1001+) to avoid conflicts with core PostgreSQL
 * tranches.
 *
 * Using fixed tranche IDs is critical for supporting large numbers of indexes
 * (e.g., partitioned tables with 500+ partitions). If we called
 * LWLockNewTrancheId() for each index, we would quickly exhaust the available
 * tranche IDs and get "too many LWLocks taken" errors.
 */
#define TP_TRANCHE_STRING		1001
#define TP_TRANCHE_POSTING		1002
#define TP_TRANCHE_CORPUS		1003
#define TP_TRANCHE_DOC_LENGTHS	1004
#define TP_TRANCHE_INDEX_LOCK	1005
#define TP_TRANCHE_BUILD_DSA	1006
#define TP_TRANCHE_GLOBAL_DSA	1007
#define TP_TRANCHE_REGISTRY		1008
#define TP_TRANCHE_POSTING_LOCK 1009

/*
 * Global GUC variables declared in mod.c
 * Note: tp_relopt_kind is declared in index.c as it requires
 * access/reloptions.h
 */
extern bool tp_log_scores;
extern int	tp_bulk_load_threshold;
extern int	tp_memtable_spill_threshold;
extern int	tp_segments_per_level;
