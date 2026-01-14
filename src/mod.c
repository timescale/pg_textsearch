/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * mod.c - Extension initialization and GUC registration
 */
#include <postgres.h>

#include <access/relation.h>
#include <access/reloptions.h>
#include <access/xact.h>
#include <catalog/dependency.h>
#include <catalog/objectaccess.h>
#include <catalog/pg_class_d.h>
#include <miscadmin.h>
#include <pg_config.h>
#include <storage/ipc.h>
#include <storage/shmem.h>
#include <utils/guc.h>
#include <utils/inval.h>

#include "am/am.h"
#include "constants.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "planner/hooks.h"
#include "query/score.h"
#include "state/registry.h"
#include "state/state.h"

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(.name = "pg_textsearch", .version = "0.0.6-dev");
#else
PG_MODULE_MAGIC;
#endif

/* Relation options for Tapir indexes */
relopt_kind tp_relopt_kind;

/* External variable from limits module */
extern int tp_default_limit;

/* Global variable for score logging */
bool tp_log_scores = false;

/* Global variable for BMW stats logging - declared in query/score.c */
bool tp_log_bmw_stats = false;

/* Global variable to enable/disable BMW optimization - declared in score.c */
bool tp_enable_bmw = true;

/* Global variable for bulk load spill threshold (0 = disabled) */
int tp_bulk_load_threshold = TP_DEFAULT_BULK_LOAD_THRESHOLD;

/* Global variable for memtable spill threshold (0 = disabled) */
int tp_memtable_spill_threshold = TP_DEFAULT_MEMTABLE_SPILL_THRESHOLD;

/* Global variable for segments per level before compaction */
int tp_segments_per_level = TP_DEFAULT_SEGMENTS_PER_LEVEL;

/* Global variable for segment compression (on by default - benchmarks show
 * compression improves both size and query performance)
 */
bool tp_compress_segments = true;

/* Previous object access hook */
static object_access_hook_type prev_object_access_hook = NULL;

/* Previous shared memory startup hook */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Shared memory request hook */
static shmem_request_hook_type prev_shmem_request_hook = NULL;

/* Shared memory size calculation */
static void tp_shmem_request(void);

/* Shared memory startup hook */
static void tp_shmem_startup(void);

/* Object access hook for DROP INDEX detection */
static void tp_object_access(
		ObjectAccessType access,
		Oid				 classId,
		Oid				 objectId,
		int				 subId,
		void			*arg);

/* Transaction callback to release index locks */
static void tp_xact_callback(XactEvent event, void *arg);

/*
 * Extension entry point - called when the extension is loaded
 */
void
_PG_init(void)
{
	/* Initialize on first use - don't require shared_preload_libraries */

	/*
	 * Define GUC parameters
	 */
	DefineCustomIntVariable(
			"pg_textsearch.default_limit",
			"Default limit for BM25 queries when no LIMIT is detected",
			"Controls the maximum number of documents to process when no "
			"LIMIT clause is present",
			&tp_default_limit,
			TP_DEFAULT_QUERY_LIMIT, /* default 1000 */
			1,						/* min 1 */
			TP_MAX_QUERY_LIMIT,		/* max 100k */
			PGC_USERSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomBoolVariable(
			"pg_textsearch.log_scores",
			"Log BM25 scores during index scans",
			"When enabled, logs the BM25 score for each document returned "
			"during index scans. Useful for debugging score calculation.",
			&tp_log_scores,
			false,		 /* default off */
			PGC_USERSET, /* Can be changed per session */
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomBoolVariable(
			"pg_textsearch.log_bmw_stats",
			"Log Block-Max WAND statistics during queries",
			"When enabled, logs blocks scanned/skipped and documents scored "
			"for each query. Useful for understanding BMW optimization.",
			&tp_log_bmw_stats,
			false,		 /* default off */
			PGC_USERSET, /* Can be changed per session */
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomBoolVariable(
			"pg_textsearch.enable_bmw",
			"Enable Block-Max WAND query optimization",
			"When enabled, uses block-level upper bounds to skip "
			"non-contributing blocks. Disable for benchmark comparison.",
			&tp_enable_bmw,
			true,		 /* default on */
			PGC_USERSET, /* Can be changed per session */
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomIntVariable(
			"pg_textsearch.bulk_load_threshold",
			"Terms per transaction to trigger memtable spill",
			"When this many terms are added in a single transaction, spill to "
			"disk at transaction end. Set to 0 to disable.",
			&tp_bulk_load_threshold,
			TP_DEFAULT_BULK_LOAD_THRESHOLD, /* default 100K */
			0,								/* min 0 (disabled) */
			INT_MAX,						/* max INT_MAX */
			PGC_USERSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomIntVariable(
			"pg_textsearch.memtable_spill_threshold",
			"Posting entries to trigger memtable spill",
			"When the memtable reaches this many posting entries, spill to "
			"disk at transaction end. Set to 0 to disable.",
			&tp_memtable_spill_threshold,
			TP_DEFAULT_MEMTABLE_SPILL_THRESHOLD, /* default 32M */
			0,									 /* min 0 (disabled) */
			INT_MAX,							 /* max INT_MAX */
			PGC_USERSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomIntVariable(
			"pg_textsearch.segments_per_level",
			"Segments per level before compaction",
			"When a level reaches this many segments, they are merged into "
			"a single segment at the next level.",
			&tp_segments_per_level,
			TP_DEFAULT_SEGMENTS_PER_LEVEL, /* default 8 */
			2,							   /* min 2 */
			64,							   /* max 64 */
			PGC_USERSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomBoolVariable(
			"pg_textsearch.compress_segments",
			"Enable compression for new segment blocks",
			"When enabled, posting blocks in new segments are compressed "
			"using delta encoding and bitpacking. Reduces index size and "
			"improves query performance by reducing I/O.",
			&tp_compress_segments,
			true,		 /* default on - benchmarks show net benefit */
			PGC_USERSET, /* Can be changed per session */
			0,
			NULL,
			NULL,
			NULL);

	/*
	 * Initialize index access method options
	 */
	tp_relopt_kind = add_reloption_kind();
	add_string_reloption(
			tp_relopt_kind,
			"text_config",
			"Text search configuration for Tapir index",
			NULL,
			NULL,
			NoLock);
	add_real_reloption(
			tp_relopt_kind,
			"k1",
			"BM25 k1 parameter",
			TP_DEFAULT_K1,
			0.1,
			10.0,
			NoLock);
	add_real_reloption(
			tp_relopt_kind,
			"b",
			"BM25 b parameter",
			TP_DEFAULT_B,
			0.0,
			1.0,
			NoLock);

	/*
	 * Install shared memory hooks (needed for registry)
	 */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook		= tp_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook		= tp_shmem_startup;

	/* Install object access hook for DROP INDEX detection */
	prev_object_access_hook = object_access_hook;
	object_access_hook		= tp_object_access;

	/* Register transaction callback to release index locks at transaction end
	 */
	RegisterXactCallback(tp_xact_callback, NULL);

	/* Install planner hook for implicit index resolution */
	tp_planner_hook_init();
}

/*
 * Object access hook - handle DROP INDEX
 */
static void
tp_object_access(
		ObjectAccessType access,
		Oid				 classId,
		Oid				 objectId,
		int				 subId,
		void			*arg)
{
	(void)arg; /* unused - we don't care about drop flags */

	/* Call previous hook if exists */
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	/* We only care about DROP events on relations (indexes are relations) */
	if (access == OAT_DROP && classId == RelationRelationId && subId == 0)
	{
		/*
		 * Always cleanup our indexes regardless of drop flags.
		 * PERFORM_DELETION_INTERNAL is set for cascade drops (e.g., DROP
		 * TABLE dropping its indexes) but we still need to free registry
		 * entries and shared memory in those cases.
		 */

		/* Check if this is one of our indexes */
		if (!tp_registry_is_registered(objectId))
			return;

		/* Cleanup shared memory and unregister from registry */
		tp_cleanup_index_shared_memory(objectId);
	}
}

/*
 * Shared memory request hook - calculate and request shared memory
 */
static void
tp_shmem_request(void)
{
	/* Call previous hook first if it exists */
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	/* Request shared memory for registry (includes DSA control) */
	tp_registry_init();
}

/*
 * Shared memory startup hook - no-op since registry initializes lazily
 */
static void
tp_shmem_startup(void)
{
	/* Call previous hook first if it exists */
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* Initialize the registry in shared memory (includes DSA control) */
	tp_registry_shmem_startup();
}

/*
 * Transaction callback - release index locks at transaction end
 * and check for bulk load auto-spill at pre-commit
 */
static void
tp_xact_callback(XactEvent event, void *arg __attribute__((unused)))
{
	switch (event)
	{
	case XACT_EVENT_PRE_COMMIT:
	case XACT_EVENT_PARALLEL_PRE_COMMIT:
		/*
		 * Check for bulk load auto-spill before commit.
		 * If any index had a large number of terms added this transaction,
		 * spill to disk to prevent unbounded memory growth.
		 */
		tp_bulk_load_spill_check();
		break;

	case XACT_EVENT_COMMIT:
	case XACT_EVENT_PARALLEL_COMMIT:
		/* Release all index locks held by this backend */
		tp_release_all_index_locks();
		/* Reset bulk load counters for next transaction */
		tp_reset_bulk_load_counters();
		break;

	case XACT_EVENT_ABORT:
	case XACT_EVENT_PARALLEL_ABORT:
		/* Clean up any in-progress index builds (private DSA) */
		tp_cleanup_build_mode_on_abort();
		/* Release all index locks held by this backend */
		tp_release_all_index_locks();
		/* Reset bulk load counters for next transaction */
		tp_reset_bulk_load_counters();
		break;

	case XACT_EVENT_PRE_PREPARE:
	case XACT_EVENT_PREPARE:
		/* Nothing to do for these events */
		break;
	}
}
