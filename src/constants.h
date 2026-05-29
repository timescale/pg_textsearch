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
				* with the on-disk memtable design, see issue #374) */

/*
 * Page format versions - bump when on-disk format changes.
 * Each page type has its own version for independent evolution.
 */
/*
 * v7: on-disk memtable redesign (issue #374).  Appends
 * memtable_head_blkno and memtable_tail_blkno at the end of
 * TpIndexMetaPageData and retires the docid recovery pages.
 *
 * v6 is read-compatible (issue #383): tp_get_metapage accepts a
 * v6 layout in place and normalizes the missing chain head/tail
 * to InvalidBlockNumber.  The first metapage mutation upgrades
 * the on-disk page to v7 atomically inside the surrounding
 * GenericXLog record (no extra WAL).
 *
 * A v6 index's only source of pending (unspilled) data is the
 * retired docid recovery chain anchored by
 * (now-renamed) _unused_docid_page: a v1.2.x clean shutdown
 * always spilled and cleared this pointer, but a SIGKILL'd
 * v1.2.x cluster could leave a non-Invalid pointer to ctids
 * that the v7 binary cannot re-tokenize.  Such an index opens
 * fine; the first metapage mutation (via
 * tp_metapage_upgrade_to_current) emits a LOG-level forensic
 * message recording the orphaned pointer and then clears it.
 * Operators who suspect lost in-flight documents from a SIGKILL
 * should REINDEX.  Indexes from a clean v1.2.x shutdown have
 * _unused_docid_page == InvalidBlockNumber and upgrade silently
 * with no operator intervention.
 *
 * v5 and below are not read-compatible.  v5 -> v6 changed BMW
 * scoring semantics with no on-disk-struct change, but
 * pre-v0.5.0 indexes carry an older segment format the v7
 * binary cannot read; those continue to require REINDEX.
 */
#define TP_METAPAGE_VERSION	   7
#define TP_METAPAGE_VERSION_V6 6 /* read-compatible (issue #383) */
#define TP_PAGE_INDEX_VERSION  1 /* Page index format version */
/* Initial version (introduced with the on-disk memtable design) */
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

/*
 * Default for pg_textsearch.memtable_pages_threshold (on-disk
 * memtable auto-spill trigger).  64 chain pages * 8 KiB = ~512
 * KiB of memtable per index before the next insert spills to an
 * L0 segment.  Tuned for low latency without producing runt L0
 * segments under typical insert traffic; 0 disables auto-spill.
 */
#define TP_DEFAULT_MEMTABLE_PAGES_THRESHOLD 64

/*
 * Lower bound on chain pages for VACUUM-cleanup and shutdown-hook
 * spills.  Below this, leaving the documents on the in-relation
 * memtable chain is cheaper than writing a runt L0 segment — LSM
 * compaction would spend more work consolidating tiny segments
 * than the chain pages cost to scan during reads.
 */
#define TP_MIN_SPILL_PAGES 2

/*
 * Default for pg_textsearch.memory_limit (in kilobytes).  Matches the
 * v1 in-memory memtable's 2 GiB ceiling.  Used as both the GUC default
 * and the initial value of the backing variable so the two cannot
 * drift.  A value of 0 means "no limit" (cache grows until backend
 * memory is exhausted); the cache implementation will treat any
 * non-zero value as a three-tier budget (per-index soft = limit/8,
 * global soft = limit/2, global hard = limit).
 */
#define TP_DEFAULT_MEMORY_LIMIT_KB (2 * 1024 * 1024)

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
 * In-memory memtable cache LWLocks (see docs/memtable_cache.md).
 * apply_lock serializes cache mutators (reader catchup, cold build,
 * spill catchup, tp_cache_clear).  lock is the cache lifetime lock,
 * held SHARED for the lifetime of a served TpDataSource and EXCL only
 * for drop / evict.
 */
#define TP_TRANCHE_CACHE_APPLY_LOCK 1010
#define TP_TRANCHE_CACHE_LOCK		1011

/*
 * Global eviction mutex tranche (see docs/memtable_cache.md
 * §"Memory cap (3 tiers)").  Serializes cache evictions across
 * backends.  Acquired EXCL by evict_largest and by index cleanup
 * (DROP INDEX path) to prevent races between an in-flight victim
 * inspection and a concurrent dsa_free of the victim's shared
 * state.
 */
#define TP_TRANCHE_EVICTION_MUTEX 1012

/*
 * Global GUC variables declared in mod.c
 * Note: tp_relopt_kind is declared in index.c as it requires
 * access/reloptions.h
 */
extern bool tp_log_scores;
extern int	tp_bulk_load_threshold;
extern int	tp_memtable_pages_threshold;
extern int	tp_segments_per_level;
