/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * mod.c - Extension initialization and GUC registration
 */
#include <postgres.h>

#include <access/htup_details.h>
#include <access/relation.h>
#include <access/reloptions.h>
#include <access/table.h>
#include <access/xact.h>
#include <catalog/dependency.h>
#include <catalog/heap.h>
#include <catalog/index.h>
#include <catalog/namespace.h>
#include <catalog/objectaccess.h>
#include <catalog/partition.h>
#include <catalog/pg_authid_d.h>
#include <catalog/pg_class_d.h>
#include <catalog/pg_database.h>
#include <catalog/pg_inherits.h>
#include <catalog/pg_namespace_d.h>
#include <commands/dbcommands.h>
#include <commands/defrem.h>
#include <commands/tablecmds.h>
#include <commands/tablespace.h>
#include <fmgr.h>
#include <limits.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/parsenodes.h>
#include <nodes/pg_list.h>
#include <pg_config.h>
#include <storage/ipc.h>
#include <storage/lmgr.h>
#include <storage/shmem.h>
#include <tcop/utility.h>
#include <utils/acl.h>
#include <utils/guc.h>
#include <utils/inval.h>
#include <utils/lsyscache.h>
#include <utils/relcache.h>
#include <utils/snapmgr.h>
#include <utils/syscache.h>

#include "access/am.h"
#include "constants.h"
#include "index/compaction_job.h"
#include "index/compaction_request.h"
#include "index/metapage.h"
#include "index/registry.h"
#include "index/state.h"
#include "planner/hooks.h"
#include "scoring/bm25.h"

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(.name = "pg_textsearch", .version = "1.5.0-dev");
#else
PG_MODULE_MAGIC;
#endif

/* Relation options for Tapir indexes */
relopt_kind tp_relopt_kind;

/* External variable from limits module */
extern int tp_default_limit;

/* Library version string for stale binary detection */
static char *tp_library_version = NULL;

/* Global variable for score logging */
bool tp_log_scores = false;

/* Global variable for BMW stats logging - declared in query/score.c */
bool tp_log_bmw_stats = false;

/* Global variable for bulk load spill threshold (0 = disabled) */
int tp_bulk_load_threshold = TP_DEFAULT_BULK_LOAD_THRESHOLD;

/*
 * Memtable v2 (issue #374) auto-spill: chain pages per index
 * before the next insert triggers a spill (0 = disabled).
 */
int tp_memtable_pages_threshold = TP_DEFAULT_MEMTABLE_PAGES_THRESHOLD;

/* Global variable for segments per level before compaction */
int tp_segments_per_level = TP_DEFAULT_SEGMENTS_PER_LEVEL;

/* Conservative size budget for newly merged multi-source segments. */
int tp_max_segment_size_mb = TP_DEFAULT_SEGMENT_SIZE_MB;

static const relopt_enum_elt_def compaction_mode_options[] =
		{{"inline", TP_COMPACTION_INLINE},
		 {"background", TP_COMPACTION_BACKGROUND},
		 {"manual", TP_COMPACTION_MANUAL},
		 {(const char *)NULL, 0}};

/* Global variable for segment compression (on by default - benchmarks show
 * compression improves both size and query performance)
 */
bool tp_compress_segments = true;

/*
 * Selectivity-seeded top-K for filtered BM25 search.
 * tp_filtered_seed gates the optimization in tp_costestimate;
 * tp_filtered_seed_margin scales the seed
 * (ceil(margin * user_limit / filter_selectivity)).
 */
bool   tp_filtered_seed		   = true;
double tp_filtered_seed_margin = TP_DEFAULT_FILTERED_SEED_MARGIN;

/*
 * Memtable shared-memory cache enable flag.  Gates the read-path
 * chooser (tp_memtable_source_create_for_read) on the cache vs
 * the on-disk chain.  Defaults to on: tp_spill_finalize bumps
 * the per-index spill_generation and calls tp_cache_clear,
 * closing the cross-spill staleness window that would otherwise
 * force opt-in.  Standbys still bypass the cache via
 * RecoveryInProgress() in the chooser.
 */
bool tp_memtable_cache_enabled = true;

/*
 * Debug GUC for the in-memory memtable cache.  When enabled the
 * read-path chooser logs cache state transitions (apply OK /
 * BUDGET_EXCEEDED / cold_build OK / RETRY / ABORT / fall back to
 * chain).  Off by default; intended for development and tests
 * that need to assert which path served a query.
 */
bool tp_log_cache_state = false;

/* Debug: trigger PANIC after spill finalize for crash-safety testing */
bool tp_debug_panic_after_spill_finalize = false;

/* Per-level segment capacity; the debug GUC may lower it in tests. */
int tp_max_segments_per_level = PG_UINT16_MAX;

/*
 * Soft+hard memory budget for the in-memory memtable cache, in
 * kilobytes.  Restored from v1; scaffolding only in this build —
 * no consumer reads this yet.  Default and bounds live in
 * constants.h (TP_DEFAULT_MEMORY_LIMIT_KB) so the GUC registration
 * and the backing-variable initializer cannot drift.
 */
int tp_memory_limit_kb = TP_DEFAULT_MEMORY_LIMIT_KB;

/* Previous object access hook */
static object_access_hook_type prev_object_access_hook = NULL;

/* Previous shared memory startup hook */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Shared memory request hook */
static shmem_request_hook_type prev_shmem_request_hook = NULL;

/* Previous ProcessUtility hook */
static ProcessUtility_hook_type prev_process_utility_hook = NULL;

typedef struct TpReindexTarget
{
	Oid			  heap_oid;
	Oid			  namespace_oid;
	char		 *index_name;
	Oid			  index_oid;
	Oid			  tablespace_oid;
	RelFileNumber relfilenumber;
	char		 *lineage;
	char		 *schedule;
	bool		  lineage_backfilled;
} TpReindexTarget;

typedef struct TpReindexState
{
	MemoryContext		   context;
	List				  *targets;
	bool				   reconciling;
	bool				   defer_reconciliation;
	struct TpReindexState *previous;
} TpReindexState;

typedef struct TpCreateIndexState
{
	MemoryContext			   context;
	Oid						   heap_oid;
	List					  *created_indexes;
	int						   utility_depth;
	struct TpCreateIndexState *previous;
} TpCreateIndexState;

typedef struct TpReindexCandidate
{
	TpReindexState	*state;
	TpReindexTarget *target;
	Oid				 index_oid;
} TpReindexCandidate;

typedef struct TpOwnerChangeTarget
{
	Oid	  index_oid;
	char *schedule;
} TpOwnerChangeTarget;

/*
 * Concurrent and partitioned REINDEX commit inside ProcessUtility.  Keep a
 * stack of invocation-owned targets across those commits so each replacement
 * can be reconciled in the transaction that publishes it.
 */
static TpReindexState *tp_reindex_states = NULL;

/*
 * CREATE INDEX CONCURRENTLY also commits inside ProcessUtility.  The object
 * access hook records only indexes created by the matching utility invocation.
 */
static TpCreateIndexState *tp_create_index_states	= NULL;
static int				   tp_process_utility_depth = 0;

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

/* Subtransaction callback for savepoint rollback cleanup */
static void tp_subxact_callback(
		SubXactEvent	 event,
		SubTransactionId mySubid,
		SubTransactionId parentSubid,
		void			*arg);

/* ProcessUtility hook for tracking CREATE INDEX USING bm25 */
static void tp_process_utility(
		PlannedStmt			 *pstmt,
		const char			 *queryString,
		bool				  readOnlyTree,
		ProcessUtilityContext context,
		ParamListInfo		  params,
		QueryEnvironment	 *queryEnv,
		DestReceiver		 *dest,
		QueryCompletion		 *qc);
static void tp_process_utility_impl(
		PlannedStmt			 *pstmt,
		const char			 *queryString,
		bool				  readOnlyTree,
		ProcessUtilityContext context,
		ParamListInfo		  params,
		QueryEnvironment	 *queryEnv,
		DestReceiver		 *dest,
		QueryCompletion		 *qc);
static void tp_reconcile_reindex_states(bool include_deferred);

static void
tp_validate_compaction_lineage(const char *lineage)
{
	if (lineage == NULL)
		return;

	if (strlen(lineage) != TP_COMPACTION_LINEAGE_BYTES * 2)
		goto invalid;

	for (Size i = 0; lineage[i] != '\0'; i++)
		if (!((lineage[i] >= '0' && lineage[i] <= '9') ||
			  (lineage[i] >= 'a' && lineage[i] <= 'f')))
			goto invalid;

	return;

invalid:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid background compaction lineage")));
}

/*
 * Extension entry point - called when the extension is loaded
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_textsearch must be loaded via "
						"shared_preload_libraries"),
				 errhint("Add '%s' to shared_preload_libraries "
						 "in postgresql.conf and restart the "
						 "server.",
						 "pg_textsearch")));

	/*
	 * Define GUC parameters
	 */
	DefineCustomStringVariable(
			"pg_textsearch.library_version",
			"Version of the loaded pg_textsearch shared library",
			NULL,
			&tp_library_version,
			PG_TEXTSEARCH_VERSION,
			PGC_INTERNAL,
			0,
			NULL,
			NULL,
			NULL);

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
			false,	   /* default off */
			PGC_SUSET, /* superuser-only: prevents GUC persistence
						* in connection-pooled environments */
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
			false,	   /* default off */
			PGC_SUSET, /* superuser-only: prevents GUC persistence
						* in connection-pooled environments */
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
			PGC_SUSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomIntVariable(
			"pg_textsearch.memtable_pages_threshold",
			"Chain pages to trigger memtable spill",
			"When the on-disk memtable chain reaches this many pages, "
			"spill to an L0 segment at the next insert.  Each page is "
			"8 KiB.  Set to 0 to disable auto-spill (chain grows until "
			"VACUUM or manual bm25_spill_index()).",
			&tp_memtable_pages_threshold,
			TP_DEFAULT_MEMTABLE_PAGES_THRESHOLD, /* default 64 */
			0,									 /* min 0 (disabled) */
			INT_MAX,							 /* max */
			PGC_SUSET,
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
			PGC_SUSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomIntVariable(
			"pg_textsearch.max_segment_size",
			"Maximum conservative size of a merged segment.",
			"Bounds newly merged multi-source segments. A larger existing "
			"segment remains an uncombinable singleton.",
			&tp_max_segment_size_mb,
			TP_DEFAULT_SEGMENT_SIZE_MB,
			TP_MIN_SEGMENT_SIZE_MB,
			TP_MAX_SEGMENT_SIZE_MB,
			PGC_SUSET,
			GUC_UNIT_MB,
			NULL,
			NULL,
			NULL);

	DefineCustomStringVariable(
			"pg_textsearch.background_compaction_schedule",
			"Default schedule for managed background compaction.",
			NULL,
			&tp_background_compaction_schedule,
			"*/5 * * * *",
			PGC_SUSET,
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

	DefineCustomBoolVariable(
			"pg_textsearch.filtered_seed",
			"Seed the BM25 top-K from estimated filter selectivity.",
			"When a filter (e.g. a facet WHERE clause) sits above a BM25 "
			"top-k index scan, seed the scan's internal top-K to "
			"ceil(margin * LIMIT / selectivity) so a single scoring pass "
			"usually surfaces enough matching rows, avoiding the executor's "
			"backoff re-drives.  Results are identical either way; the "
			"backoff remains the correctness safety net.",
			&tp_filtered_seed,
			true, /* default on */
			PGC_USERSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomRealVariable(
			"pg_textsearch.filtered_seed_margin",
			"Margin applied when seeding the BM25 top-K from selectivity.",
			"Seed = ceil(margin * LIMIT / selectivity).  A higher margin "
			"captures the true top-k matching rows in one scoring pass more "
			"often, at the cost of scoring deeper.  Only used when "
			"pg_textsearch.filtered_seed is on.",
			&tp_filtered_seed_margin,
			TP_DEFAULT_FILTERED_SEED_MARGIN, /* default 3.0 */
			TP_MIN_FILTERED_SEED_MARGIN,	 /* min 1.0 */
			TP_MAX_FILTERED_SEED_MARGIN,	 /* max 1000.0 */
			PGC_USERSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomBoolVariable(
			"pg_textsearch.memtable_cache_enabled",
			"Enable the in-memory memtable cache for queries.",
			"When enabled, queries are served from a derived "
			"shared-memory cache rather than walking the on-disk "
			"memtable chain.  The chain remains the source of "
			"truth.  Default is on: queries against the chain are "
			"served by a derived cache so per-term posting lookups "
			"don't pay the O(records) chain walk.  Disable to "
			"force every query through the on-disk chain.  "
			"Standbys always use the chain regardless of this "
			"setting (RecoveryInProgress disables the cache).",
			&tp_memtable_cache_enabled,
			true,
			PGC_USERSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomBoolVariable(
			"pg_textsearch.log_cache_state",
			"Log in-memory memtable cache state transitions.",
			"When enabled, the read-path chooser emits LOG messages "
			"for each cache apply outcome (OK / BUDGET_EXCEEDED / "
			"cold_build / RETRY / ABORT / fall back to chain).  "
			"Intended for development and observability.",
			&tp_log_cache_state,
			false,
			PGC_USERSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomBoolVariable(
			"pg_textsearch.debug_panic_after_spill_finalize",
			"Trigger PANIC after spill finalize for crash-safety testing.",
			"When enabled, forces a server crash immediately after "
			"tp_spill_finalize completes. Used only for regression "
			"testing crash-safe spill ordering.",
			&tp_debug_panic_after_spill_finalize,
			false,
			PGC_SUSET, /* superuser-only: forces a server-wide PANIC,
						* so unprivileged roles must not reach it */
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomIntVariable(
			"pg_textsearch.debug_segment_count_limit",
			"Set the maximum persisted segment count per level.",
			"Testing-only limit for exercising segment-count overflow.",
			&tp_max_segments_per_level,
			PG_UINT16_MAX,
			1,
			PG_UINT16_MAX,
			PGC_SUSET,
			0,
			NULL,
			NULL,
			NULL);

	DefineCustomIntVariable(
			"pg_textsearch.memory_limit",
			"Approximate shared-memory budget for the in-memory memtable "
			"cache.",
			"Applied as a three-tier budget: the per-index limit/8 "
			"per-record growth guard returns BUDGET_EXCEEDED and "
			"falls back to the chain; the global limit/2 threshold "
			"tries to evict the largest non-caller cache; and limit "
			"is an approximate admission threshold for catch-up "
			"and cold builds.  Admitted or concurrent work may "
			"increase estimated usage past the limit.  A value of "
			"0 means unlimited.",
			&tp_memory_limit_kb,
			TP_DEFAULT_MEMORY_LIMIT_KB,
			0,
			INT_MAX,
			PGC_SIGHUP,
			GUC_UNIT_KB,
			NULL,
			NULL,
			NULL);

	/*
	 * Reserve the pg_textsearch.* GUC prefix so unknown settings
	 * (typos, or GUCs removed in a future release) produce a
	 * warning at server start rather than being silently ignored.
	 */
	MarkGUCPrefixReserved("pg_textsearch");

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
	 * ShareUpdateExclusiveLock: the value is read after a spill and
	 * changes no on-disk structure, so ALTER INDEX need not block.
	 */
	add_enum_reloption(
			tp_relopt_kind,
			"compaction",
			"Spill-time segment compaction policy",
			(relopt_enum_elt_def *)compaction_mode_options,
			TP_COMPACTION_INLINE,
			"Valid values are \"inline\", \"background\" and \"manual\".",
			ShareUpdateExclusiveLock);
	add_string_reloption(
			tp_relopt_kind,
			"compaction_schedule",
			"Managed background compaction schedule",
			NULL,
			NULL,
			ShareUpdateExclusiveLock);
	add_string_reloption(
			tp_relopt_kind,
			"compaction_lineage",
			"Internal managed background compaction lineage",
			NULL,
			tp_validate_compaction_lineage,
			ShareUpdateExclusiveLock);

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

	/* Register subtransaction callback for savepoint rollback cleanup */
	RegisterSubXactCallback(tp_subxact_callback, NULL);

	/* Install planner hook for implicit index resolution */
	tp_planner_hook_init();

	/* Install ProcessUtility hook for partitioned build tracking */
	prev_process_utility_hook = ProcessUtility_hook;
	ProcessUtility_hook		  = tp_process_utility;
}

static TpCreateIndexState *
tp_create_index_tracking_begin(Oid heap_oid)
{
	MemoryContext		caller_context = CurrentMemoryContext;
	MemoryContext		context;
	TpCreateIndexState *state;

	context = AllocSetContextCreate(
			TopMemoryContext,
			"pg_textsearch create index",
			ALLOCSET_SMALL_SIZES);
	PG_TRY();
	{
		state				 = MemoryContextAllocZero(context, sizeof(*state));
		state->context		 = context;
		state->heap_oid		 = heap_oid;
		state->utility_depth = tp_process_utility_depth;
		state->previous		 = tp_create_index_states;
		tp_create_index_states = state;
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(caller_context);
		MemoryContextDelete(context);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return state;
}

static void
tp_create_index_tracking_end(TpCreateIndexState *state)
{
	MemoryContext context;

	if (state == NULL)
		return;

	Assert(tp_create_index_states == state);
	context				   = state->context;
	tp_create_index_states = state->previous;
	MemoryContextDelete(context);
}

/*
 * Object access hook - record CREATE INDEX objects and handle DROP INDEX.
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

	if (access == OAT_POST_CREATE && classId == RelationRelationId &&
		subId == 0 && tp_create_index_states != NULL &&
		tp_create_index_states->utility_depth == tp_process_utility_depth)
	{
		MemoryContext old_context;

		old_context = MemoryContextSwitchTo(tp_create_index_states->context);
		tp_create_index_states->created_indexes = list_append_unique_oid(
				tp_create_index_states->created_indexes, objectId);
		MemoryContextSwitchTo(old_context);
	}

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
 * Shared memory startup hook - initialize the registry
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
 * Transaction callback - reconcile REINDEX replacements, release index
 * locks at transaction end, check for bulk load auto-spill at pre-commit,
 * and dispatch any compaction requests those spills registered.
 *
 * The spill check runs first: a spill can register a request, so
 * flushing afterwards is what lets that request reach the callback in
 * the same transaction.  Parallel workers deliberately do not flush;
 * dispatch runs subtransactions and SPI, which a worker must not do.
 */
static void
tp_xact_callback(XactEvent event, void *arg pg_attribute_unused())
{
	switch (event)
	{
	case XACT_EVENT_PRE_COMMIT:
		if (tp_reindex_states != NULL)
		{
			PushActiveSnapshot(GetLatestSnapshot());
			PG_TRY();
			{
				tp_reconcile_reindex_states(true);
			}
			PG_FINALLY();
			{
				PopActiveSnapshot();
			}
			PG_END_TRY();
		}

		/*
		 * Check for bulk load auto-spill before commit.
		 * If any index had a large number of terms added this transaction,
		 * spill to disk to prevent unbounded memory growth.
		 */
		tp_bulk_load_spill_check();
		tp_compaction_flush_requests();
		break;

	case XACT_EVENT_PARALLEL_PRE_COMMIT:
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

/*
 * Subtransaction callback - clean up index state on savepoint rollback
 *
 * When a subtransaction aborts (ROLLBACK TO SAVEPOINT), we must:
 * 1. Clean up registry/shared memory for indexes created in that
 *    subtransaction (OAT_DROP doesn't fire for subtransaction abort)
 * 2. Reset lock tracking (LWLockReleaseAll releases all locks)
 *
 * When a subtransaction commits (RELEASE SAVEPOINT), we promote
 * states to the parent subtransaction so they get cleaned up if the
 * parent later aborts.
 */
static void
tp_subxact_callback(
		SubXactEvent	 event,
		SubTransactionId mySubid,
		SubTransactionId parentSubid,
		void *arg		 pg_attribute_unused())
{
	switch (event)
	{
	case SUBXACT_EVENT_ABORT_SUB:
		tp_cleanup_subxact_abort(mySubid);
		break;

	case SUBXACT_EVENT_COMMIT_SUB:
		tp_promote_subxact_states(mySubid, parentSubid);
		break;

	case SUBXACT_EVENT_START_SUB:
	case SUBXACT_EVENT_PRE_COMMIT_SUB:
		/* Nothing to do */
		break;
	}
}

static const char *
tp_index_stmt_option(IndexStmt *stmt, const char *option_name)
{
	ListCell *lc;

	foreach (lc, stmt->options)
	{
		DefElem *option = lfirst_node(DefElem, lc);

		if (strcmp(option->defname, option_name) == 0)
			return defGetString(option);
	}

	return NULL;
}

static int
tp_index_stmt_option_count(IndexStmt *stmt, const char *option_name)
{
	ListCell *lc;
	int		  count = 0;

	foreach (lc, stmt->options)
	{
		DefElem *option = lfirst_node(DefElem, lc);

		if (strcmp(option->defname, option_name) == 0)
			count++;
	}

	return count;
}

static void
tp_index_stmt_remove_option(IndexStmt *stmt, const char *option_name)
{
	ListCell *lc;

	foreach (lc, stmt->options)
	{
		DefElem *option = lfirst_node(DefElem, lc);

		if (strcmp(option->defname, option_name) == 0)
		{
			stmt->options = foreach_delete_current(stmt->options, lc);
			return;
		}
	}
}

static void
tp_index_stmt_ensure_lineage(IndexStmt *stmt, Oid heap_oid, Oid owner_oid)
{
	char *lineage;

	if (tp_index_stmt_option(stmt, "compaction_lineage") != NULL)
		return;

	lineage		  = tp_new_available_compaction_lineage(heap_oid, owner_oid);
	stmt->options = lappend(
			stmt->options,
			makeDefElem(
					"compaction_lineage", (Node *)makeString(lineage), -1));
}

static void
tp_index_stmt_validate_supplied_lineage(
		IndexStmt *stmt, Oid heap_oid, Oid owner_oid)
{
	const char *lineage;
	int			count;

	count = tp_index_stmt_option_count(stmt, "compaction_lineage");
	if (count == 0)
		return;
	if (count > 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"compaction_lineage\" specified more "
						"than once")));

	lineage = tp_index_stmt_option(stmt, "compaction_lineage");
	tp_validate_compaction_lineage(lineage);

	if (!object_ownercheck(RelationRelationId, heap_oid, GetUserId()))
		return;

	if (tp_compaction_lineage_in_use(lineage, heap_oid, owner_oid))
	{
		/*
		 * PostgreSQL copies index reloptions while expanding CREATE TABLE
		 * LIKE.  A nested clone onto an unrelated heap needs a new lineage;
		 * top-level supplied values still retain strict collision checks.
		 */
		if (tp_process_utility_depth > 1)
		{
			tp_index_stmt_remove_option(stmt, "compaction_lineage");
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background compaction lineage is already in use")));
	}

	tp_lock_compaction_lineage(lineage);
	if (tp_compaction_lineage_in_use(lineage, heap_oid, owner_oid))
	{
		if (tp_process_utility_depth > 1)
		{
			tp_index_stmt_remove_option(stmt, "compaction_lineage");
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background compaction lineage is already in use")));
	}
}

static bool
tp_alter_index_mentions_lineage(AlterTableStmt *stmt)
{
	ListCell *lc;

	if (stmt->objtype != OBJECT_INDEX)
		return false;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);
		ListCell	  *option_lc;

		if (cmd->subtype != AT_SetRelOptions &&
			cmd->subtype != AT_ResetRelOptions &&
			cmd->subtype != AT_ReplaceRelOptions)
			continue;

		foreach (option_lc, castNode(List, cmd->def))
		{
			DefElem *option = lfirst_node(DefElem, option_lc);

			if (strcmp(option->defname, "compaction_lineage") == 0)
				return true;
		}
	}

	return false;
}

static void
tp_reject_user_lineage_alter(AlterTableStmt *stmt)
{
	LOCKMODE lockmode;
	Oid		 indexoid;
	Relation index_rel;
	bool	 is_bm25;

	if (!tp_alter_index_mentions_lineage(stmt))
		return;

	lockmode = AlterTableGetLockLevel(stmt->cmds);
	indexoid = RangeVarGetRelidExtended(
			stmt->relation,
			lockmode,
			stmt->missing_ok ? RVR_MISSING_OK : 0,
			RangeVarCallbackOwnsRelation,
			NULL);
	if (!OidIsValid(indexoid))
		return;

	index_rel = try_relation_open(indexoid, NoLock);
	if (index_rel == NULL)
		return;
	is_bm25 = (index_rel->rd_rel->relkind == RELKIND_INDEX ||
			   index_rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX) &&
			  index_rel->rd_indam != NULL &&
			  index_rel->rd_indam->ambuild == tp_build;
	relation_close(index_rel, NoLock);

	if (is_bm25)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter internal background compaction "
						"lineage")));
}

static bool
tp_background_cic_needs_preflight(IndexStmt *stmt, Relation heap_rel)
{
	const char *mode;

	if (!stmt->concurrent)
		return false;

	mode = tp_index_stmt_option(stmt, "compaction");
	if (mode == NULL || strcmp(mode, "background") != 0)
		return false;

	if (stmt->if_not_exists && stmt->idxname != NULL &&
		OidIsValid(get_relname_relid(
				stmt->idxname, RelationGetNamespace(heap_rel))))
		return false;

	return true;
}

static bool
tp_alter_index_refreshes_background(AlterTableStmt *stmt)
{
	ListCell *lc;

	if (stmt->objtype != OBJECT_INDEX)
		return false;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);
		ListCell	  *option_lc;

		if (cmd->subtype != AT_SetRelOptions &&
			cmd->subtype != AT_ResetRelOptions &&
			cmd->subtype != AT_ReplaceRelOptions)
			continue;

		foreach (option_lc, castNode(List, cmd->def))
		{
			DefElem *option = lfirst_node(DefElem, option_lc);

			if (strcmp(option->defname, "compaction") == 0 ||
				strcmp(option->defname, "compaction_schedule") == 0)
				return true;
		}
	}

	return false;
}

static bool
tp_is_background_index_relation(Relation index_rel)
{
	return index_rel->rd_rel->relkind == RELKIND_INDEX &&
		   index_rel->rd_indam != NULL &&
		   index_rel->rd_indam->ambuild == tp_build &&
		   index_rel->rd_index != NULL &&
		   tp_index_compaction_mode(index_rel) == TP_COMPACTION_BACKGROUND;
}

static bool
tp_is_physical_bm25_index_relation(Relation index_rel)
{
	return index_rel->rd_rel->relkind == RELKIND_INDEX &&
		   index_rel->rd_indam != NULL &&
		   index_rel->rd_indam->ambuild == tp_build &&
		   index_rel->rd_index != NULL && index_rel->rd_index->indisvalid &&
		   index_rel->rd_index->indisready && index_rel->rd_index->indislive;
}

static bool
tp_is_background_physical_index_relation(Relation index_rel)
{
	return tp_is_physical_bm25_index_relation(index_rel) &&
		   tp_index_compaction_mode(index_rel) == TP_COMPACTION_BACKGROUND;
}

static bool
tp_is_physical_bm25_index(Oid indexoid)
{
	Relation index_rel;
	bool	 is_bm25;

	index_rel = try_relation_open(indexoid, AccessShareLock);
	if (index_rel == NULL)
		return false;

	is_bm25 = tp_is_physical_bm25_index_relation(index_rel);
	relation_close(index_rel, AccessShareLock);
	return is_bm25;
}

static bool
tp_is_background_physical_index(Oid indexoid)
{
	Relation index_rel;
	bool	 background;

	index_rel = try_relation_open(indexoid, AccessShareLock);
	if (index_rel == NULL)
		return false;

	background = tp_is_background_physical_index_relation(index_rel);
	relation_close(index_rel, AccessShareLock);
	return background;
}

static List *
tp_physical_bm25_indexes(List *indexoids, bool background_only)
{
	List	 *result = NIL;
	ListCell *lc;

	foreach (lc, indexoids)
	{
		Oid		 indexoid = lfirst_oid(lc);
		Relation index_rel;
		bool	 matches;

		index_rel = try_relation_open(indexoid, AccessShareLock);
		if (index_rel == NULL)
			continue;
		matches = tp_is_physical_bm25_index_relation(index_rel) &&
				  (!background_only || tp_index_compaction_mode(index_rel) ==
											   TP_COMPACTION_BACKGROUND);
		relation_close(index_rel, AccessShareLock);
		if (matches)
			result = list_append_unique_oid(result, indexoid);
	}
	return result;
}

static List *
tp_all_bm25_indexes(
		Oid	  tablespace_oid,
		bool  filter_tablespace,
		List *owner_oids,
		bool  require_current_owner)
{
	Relation	class_rel;
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			bm25_am_oid = get_index_am_oid("bm25", false);
	List	   *indexoids	= NIL;

	class_rel = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(class_rel, InvalidOid, false, NULL, 0, NULL);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_class class_form = (Form_pg_class)GETSTRUCT(tuple);

		if (class_form->relkind != RELKIND_INDEX ||
			class_form->relam != bm25_am_oid ||
			(filter_tablespace &&
			 class_form->reltablespace != tablespace_oid) ||
			(owner_oids != NIL &&
			 !list_member_oid(owner_oids, class_form->relowner)) ||
			(require_current_owner &&
			 !object_ownercheck(
					 RelationRelationId, class_form->oid, GetUserId())))
			continue;

		indexoids = lappend_oid(indexoids, class_form->oid);
	}
	systable_endscan(scan);
	table_close(class_rel, AccessShareLock);
	return indexoids;
}

static Oid
tp_catalog_tablespace_oid(Oid tablespace_oid)
{
	HeapTuple		 tuple;
	Form_pg_database database;
	Oid				 catalog_oid = tablespace_oid;

	tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	database = (Form_pg_database)GETSTRUCT(tuple);
	if (tablespace_oid == database->dattablespace)
		catalog_oid = InvalidOid;
	ReleaseSysCache(tuple);
	return catalog_oid;
}

static List *
tp_role_oids(List *roles)
{
	List	 *owner_oids = NIL;
	ListCell *lc;

	foreach (lc, roles)
	{
		RoleSpec *role		= lfirst_node(RoleSpec, lc);
		Oid		  owner_oid = get_rolespec_oid(role, false);

		if (!has_privs_of_role(GetUserId(), owner_oid))
		{
			list_free(owner_oids);
			return list_make1_oid(InvalidOid);
		}
		owner_oids = list_append_unique_oid(owner_oids, owner_oid);
	}
	return owner_oids;
}

static bool
tp_can_maintain_relation(Oid relation_oid)
{
	return pg_class_aclcheck(relation_oid, GetUserId(), ACL_MAINTAIN) ==
		   ACLCHECK_OK;
}

static List *
tp_maintainable_indexes(List *indexoids, bool maintain_privilege)
{
	List	 *eligible = NIL;
	ListCell *lc;

	foreach (lc, indexoids)
	{
		Oid indexoid = lfirst_oid(lc);
		Oid heap_oid = IndexGetRelation(indexoid, true);

		if (!OidIsValid(heap_oid))
			continue;
		if (maintain_privilege
					? tp_can_maintain_relation(heap_oid)
					: object_ownercheck(
							  RelationRelationId, heap_oid, GetUserId()))
			eligible = lappend_oid(eligible, indexoid);
	}
	list_free(indexoids);
	return eligible;
}

static List *
tp_namespace_bm25_indexes(Oid namespace_oid)
{
	List	 *all_indexes = tp_all_bm25_indexes(InvalidOid, false, NIL, false);
	List	 *indexoids	  = NIL;
	ListCell *lc;

	foreach (lc, all_indexes)
	{
		Oid indexoid = lfirst_oid(lc);

		if (get_rel_namespace(indexoid) == namespace_oid)
			indexoids = lappend_oid(indexoids, indexoid);
	}
	list_free(all_indexes);
	return indexoids;
}

static List *
tp_reassign_owned_indexes(ReassignOwnedStmt *stmt)
{
	List	 *owner_oids = NIL;
	List	 *indexoids;
	ListCell *lc;
	Oid		  new_owner_oid = get_rolespec_oid(stmt->newrole, false);

	if (!has_privs_of_role(GetUserId(), new_owner_oid))
		return NIL;

	foreach (lc, stmt->roles)
	{
		RoleSpec *role		= lfirst_node(RoleSpec, lc);
		Oid		  owner_oid = get_rolespec_oid(role, false);

		if (!has_privs_of_role(GetUserId(), owner_oid))
		{
			list_free(owner_oids);
			return NIL;
		}
		owner_oids = list_append_unique_oid(owner_oids, owner_oid);
	}
	indexoids = tp_all_bm25_indexes(InvalidOid, false, owner_oids, false);
	list_free(owner_oids);
	return indexoids;
}

static List *
tp_relation_indexes_locked(Oid relation_oid, LOCKMODE lockmode)
{
	Relation relation;
	List	*indexes;

	relation = relation_open(relation_oid, lockmode);
	indexes	 = list_copy(RelationGetIndexList(relation));
	relation_close(relation, NoLock);
	return indexes;
}

static List *
tp_relation_tree_indexes_locked(Oid relation_oid, LOCKMODE lockmode)
{
	Relation  root;
	List	 *relations;
	List	 *indexes = NIL;
	ListCell *lc;

	root = relation_open(relation_oid, NoLock);
	if (root->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
	{
		indexes = list_copy(RelationGetIndexList(root));
		relation_close(root, NoLock);
		return indexes;
	}
	relation_close(root, NoLock);

	relations = find_all_inheritors(relation_oid, lockmode, NULL);
	foreach (lc, relations)
	{
		Oid		 child_oid = lfirst_oid(lc);
		Relation child;
		List	*child_indexes;

		child		  = relation_open(child_oid, NoLock);
		child_indexes = list_copy(RelationGetIndexList(child));
		relation_close(child, NoLock);
		indexes = list_concat_unique_oid(indexes, child_indexes);
	}
	list_free(relations);
	return indexes;
}

static List *
tp_index_tree_locked(Oid indexoid, LOCKMODE lockmode)
{
	Relation index_rel;
	List	*indexes;

	index_rel = index_open(indexoid, NoLock);
	if (index_rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		indexes = find_all_inheritors(indexoid, lockmode, NULL);
	else
		indexes = list_make1_oid(indexoid);
	index_close(index_rel, NoLock);
	return indexes;
}

static bool
tp_reindex_concurrently(ReindexStmt *stmt)
{
	ListCell *lc;

	foreach (lc, stmt->params)
	{
		DefElem *option = lfirst_node(DefElem, lc);

		if (strcmp(option->defname, "concurrently") == 0)
			return defGetBoolean(option);
	}

	return false;
}

typedef struct TpReindexIndexLookupState
{
	LOCKMODE table_lockmode;
	Oid		 locked_table_oid;
} TpReindexIndexLookupState;

static void
tp_reindex_index_lookup_callback(
		const RangeVar *relation, Oid relid, Oid old_relid, void *arg)
{
	TpReindexIndexLookupState *state = arg;
	Oid						   table_oid;
	char					   relkind;

	(void)relation;

	if (relid != old_relid && OidIsValid(old_relid) &&
		OidIsValid(state->locked_table_oid))
	{
		UnlockRelationOid(state->locked_table_oid, state->table_lockmode);
		state->locked_table_oid = InvalidOid;
	}

	if (!OidIsValid(relid))
		return;

	relkind = get_rel_relkind(relid);
	if (!relkind)
		return;
	if (relkind != RELKIND_INDEX && relkind != RELKIND_PARTITIONED_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index", relation->relname)));

	table_oid = IndexGetRelation(relid, true);
	if (OidIsValid(table_oid))
	{
		AclResult aclresult;

		aclresult = pg_class_aclcheck(table_oid, GetUserId(), ACL_MAINTAIN);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_INDEX, relation->relname);
	}

	if (relid != old_relid && OidIsValid(table_oid))
	{
		LockRelationOid(table_oid, state->table_lockmode);
		state->locked_table_oid = table_oid;
	}
}

static List *
tp_reindex_initial_indexes(ReindexStmt *stmt, bool *tracks_commits)
{
	TpReindexIndexLookupState lookup_state;
	LOCKMODE				  relation_lockmode;
	Oid						  relation_oid;
	char					  relkind;
	bool					  concurrently = tp_reindex_concurrently(stmt);
	List					 *indexoids;

	if (stmt->kind == REINDEX_OBJECT_SCHEMA)
	{
		Oid namespace_oid = get_namespace_oid(stmt->name, false);

		if (!object_ownercheck(
					NamespaceRelationId, namespace_oid, GetUserId()) &&
			!has_privs_of_role(GetUserId(), ROLE_PG_MAINTAIN))
		{
			*tracks_commits = false;
			return NIL;
		}
		*tracks_commits = true;
		return tp_namespace_bm25_indexes(namespace_oid);
	}

	if (stmt->kind == REINDEX_OBJECT_DATABASE)
	{
		char *database_name = get_database_name(MyDatabaseId);

		if (database_name == NULL ||
			(stmt->name != NULL && strcmp(stmt->name, database_name) != 0))
		{
			if (database_name != NULL)
				pfree(database_name);
			*tracks_commits = false;
			return NIL;
		}
		pfree(database_name);
		if (!object_ownercheck(
					DatabaseRelationId, MyDatabaseId, GetUserId()) &&
			!has_privs_of_role(GetUserId(), ROLE_PG_MAINTAIN))
		{
			*tracks_commits = false;
			return NIL;
		}
		*tracks_commits = true;
		return tp_all_bm25_indexes(InvalidOid, false, NIL, false);
	}

	if (stmt->kind != REINDEX_OBJECT_INDEX &&
		stmt->kind != REINDEX_OBJECT_TABLE)
	{
		*tracks_commits = false;
		return NIL;
	}

	if (stmt->kind == REINDEX_OBJECT_INDEX)
	{
		lookup_state.table_lockmode	  = concurrently ? ShareUpdateExclusiveLock
													 : ShareLock;
		lookup_state.locked_table_oid = InvalidOid;
		relation_lockmode			  = concurrently ? ShareUpdateExclusiveLock
													 : AccessExclusiveLock;
		relation_oid				  = RangeVarGetRelidExtended(
				 stmt->relation,
				 relation_lockmode,
				 0,
				 tp_reindex_index_lookup_callback,
				 &lookup_state);
		relkind			= get_rel_relkind(relation_oid);
		*tracks_commits = concurrently || relkind == RELKIND_PARTITIONED_INDEX;
		if (!*tracks_commits)
			return NIL;

		if (relkind == RELKIND_PARTITIONED_INDEX)
			return tp_index_tree_locked(relation_oid, ShareLock);
		return list_make1_oid(relation_oid);
	}

	relation_lockmode = concurrently ? ShareUpdateExclusiveLock : ShareLock;
	relation_oid	  = RangeVarGetRelidExtended(
			 stmt->relation,
			 relation_lockmode,
			 0,
			 RangeVarCallbackMaintainsTable,
			 NULL);
	relkind			= get_rel_relkind(relation_oid);
	*tracks_commits = concurrently || relkind == RELKIND_PARTITIONED_TABLE;
	if (!*tracks_commits)
		return NIL;

	if (relkind == RELKIND_PARTITIONED_TABLE)
		return tp_relation_tree_indexes_locked(relation_oid, ShareLock);

	indexoids = tp_relation_indexes_locked(relation_oid, NoLock);
	return indexoids;
}

static void
tp_activate_background_indexes(List *indexoids, bool refresh_default)
{
	List	 *candidates;
	List	 *targets;
	ListCell *lc;

	candidates = tp_physical_bm25_indexes(indexoids, true);
	targets	   = tp_prelock_compaction_indexes(candidates);
	list_free(candidates);
	foreach (lc, targets)
	{
		Oid indexoid = lfirst_oid(lc);

		if (tp_is_background_physical_index(indexoid))
			tp_compaction_job_activate(indexoid, refresh_default);
	}
	list_free(targets);
}

static void
tp_prelock_owner_change_heaps(List *indexoids)
{
	List	 *heap_oids = NIL;
	ListCell *lc;
	Oid		  previous = InvalidOid;

	foreach (lc, indexoids)
	{
		Oid heap_oid = IndexGetRelation(lfirst_oid(lc), true);

		if (OidIsValid(heap_oid))
			heap_oids = list_append_unique_oid(heap_oids, heap_oid);
	}
	list_sort(heap_oids, list_oid_cmp);
	foreach (lc, heap_oids)
	{
		Oid heap_oid = lfirst_oid(lc);

		if (heap_oid == previous)
			continue;
		LockRelationOid(heap_oid, AccessExclusiveLock);
		previous = heap_oid;
	}
	list_free(heap_oids);
}

static List *
tp_capture_owner_change_targets(List *indexoids)
{
	List	 *targets = NIL;
	ListCell *lc;

	foreach (lc, indexoids)
	{
		Oid					 indexoid = lfirst_oid(lc);
		TpOwnerChangeTarget *target;

		if (!tp_is_background_physical_index(indexoid))
			continue;

		target			  = palloc(sizeof(*target));
		target->index_oid = indexoid;
		target->schedule  = tp_compaction_job_schedule(indexoid, false);
		targets			  = lappend(targets, target);
	}
	return targets;
}

static void
tp_activate_owner_change_targets(List *targets)
{
	ListCell *lc;

	foreach (lc, targets)
	{
		TpOwnerChangeTarget *target = lfirst(lc);

		if (tp_is_background_physical_index(target->index_oid))
			tp_compaction_job_activate_with_schedule(
					target->index_oid, target->schedule);
	}
}

static void
tp_free_owner_change_targets(List *targets)
{
	ListCell *lc;

	foreach (lc, targets)
	{
		TpOwnerChangeTarget *target = lfirst(lc);

		pfree(target->schedule);
		pfree(target);
	}
	list_free(targets);
}

static List *
tp_created_index_tree_locked(List *created_indexes, Oid heap_oid)
{
	List	 *result = NIL;
	ListCell *lc;

	foreach (lc, created_indexes)
	{
		Oid		 indexoid = lfirst_oid(lc);
		Relation index_rel;
		List	*index_tree;
		List	*ancestors = NIL;
		Oid		 index_heap_oid;
		bool	 belongs;

		index_rel = try_relation_open(indexoid, AccessShareLock);
		if (index_rel == NULL)
			continue;
		if ((index_rel->rd_rel->relkind != RELKIND_INDEX &&
			 index_rel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX) ||
			index_rel->rd_index == NULL)
		{
			relation_close(index_rel, AccessShareLock);
			continue;
		}

		index_heap_oid = index_rel->rd_index->indrelid;
		belongs		   = index_heap_oid == heap_oid;
		if (!belongs && get_rel_relispartition(index_heap_oid))
		{
			ancestors = get_partition_ancestors(index_heap_oid);
			belongs	  = list_member_oid(ancestors, heap_oid);
			list_free(ancestors);
		}
		if (!belongs)
		{
			relation_close(index_rel, AccessShareLock);
			continue;
		}

		if (index_rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
			index_tree = find_all_inheritors(indexoid, AccessShareLock, NULL);
		else if (index_rel->rd_rel->relkind == RELKIND_INDEX)
			index_tree = list_make1_oid(indexoid);
		else
		{
			char *index_name = pstrdup(RelationGetRelationName(index_rel));

			relation_close(index_rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not an index", index_name)));
		}

		relation_close(index_rel, NoLock);
		result = list_concat_unique_oid(result, index_tree);
	}
	return result;
}

static void
tp_reconcile_created_background_indexes(
		List	   *created_indexes,
		Oid			heap_oid,
		const char *schedule,
		const char *lineage)
{
	List	 *index_tree;
	List	 *physical_indexes;
	ListCell *lc;

	if (lineage == NULL)
		elog(ERROR, "background index has no compaction lineage");

	index_tree		 = tp_created_index_tree_locked(created_indexes, heap_oid);
	physical_indexes = tp_physical_bm25_indexes(index_tree, false);
	list_free(index_tree);
	index_tree = tp_prelock_compaction_indexes(physical_indexes);
	list_free(physical_indexes);

	foreach (lc, index_tree)
	{
		Oid indexoid = lfirst_oid(lc);

		if (!tp_is_physical_bm25_index(indexoid))
			continue;

		tp_reconcile_index_compaction_options(indexoid, schedule, lineage);
		tp_compaction_job_activate(indexoid, true);
	}
	list_free(index_tree);
}

static void
tp_reconcile_partition_index_options(Oid relation_oid)
{
	List *parent_indexes = tp_relation_indexes_locked(relation_oid, NoLock);
	ListCell *lc;

	foreach (lc, parent_indexes)
	{
		Oid		 parent_index_oid = lfirst_oid(lc);
		Relation parent_index;
		List	*index_tree;
		char	*lineage;
		char	*schedule = NULL;

		parent_index = try_relation_open(parent_index_oid, AccessShareLock);
		if (parent_index == NULL)
			continue;
		if (parent_index->rd_rel->relkind != RELKIND_PARTITIONED_INDEX ||
			tp_index_compaction_mode(parent_index) != TP_COMPACTION_BACKGROUND)
		{
			relation_close(parent_index, AccessShareLock);
			continue;
		}
		if (tp_index_compaction_lineage(parent_index) == NULL)
		{
			relation_close(parent_index, AccessShareLock);
			continue;
		}

		lineage = pstrdup(tp_index_compaction_lineage(parent_index));
		if (tp_index_compaction_schedule(parent_index) != NULL)
			schedule = pstrdup(tp_index_compaction_schedule(parent_index));
		index_tree =
				find_all_inheritors(parent_index_oid, AccessShareLock, NULL);
		relation_close(parent_index, AccessShareLock);
		tp_reconcile_created_background_indexes(
				index_tree, relation_oid, schedule, lineage);
		list_free(index_tree);
		if (schedule != NULL)
			pfree(schedule);
		pfree(lineage);
	}
	list_free(parent_indexes);
}

static void
tp_reconcile_attached_index_options(Oid parent_index_oid)
{
	Relation parent_index;
	List	*index_tree;
	Oid		 heap_oid;
	char	*lineage;
	char	*schedule = NULL;

	parent_index = relation_open(parent_index_oid, AccessShareLock);
	if (parent_index->rd_rel->relkind != RELKIND_PARTITIONED_INDEX ||
		parent_index->rd_index == NULL ||
		tp_index_compaction_mode(parent_index) != TP_COMPACTION_BACKGROUND ||
		tp_index_compaction_lineage(parent_index) == NULL)
	{
		relation_close(parent_index, AccessShareLock);
		return;
	}

	heap_oid = parent_index->rd_index->indrelid;
	lineage	 = pstrdup(tp_index_compaction_lineage(parent_index));
	if (tp_index_compaction_schedule(parent_index) != NULL)
		schedule = pstrdup(tp_index_compaction_schedule(parent_index));
	index_tree = find_all_inheritors(parent_index_oid, AccessShareLock, NULL);
	relation_close(parent_index, AccessShareLock);

	tp_reconcile_created_background_indexes(
			index_tree, heap_oid, schedule, lineage);
	list_free(index_tree);
	if (schedule != NULL)
		pfree(schedule);
	pfree(lineage);
}

static TpReindexState *
tp_reindex_tracking_begin(List *indexoids, bool defer_reconciliation)
{
	MemoryContext	caller_context = CurrentMemoryContext;
	MemoryContext	context;
	TpReindexState *state = NULL;
	ListCell	   *lc;

	context = AllocSetContextCreate(
			TopMemoryContext, "pg_textsearch reindex", ALLOCSET_SMALL_SIZES);
	PG_TRY();
	{
		state			= MemoryContextAllocZero(context, sizeof(*state));
		state->context	= context;
		state->previous = tp_reindex_states;
		state->defer_reconciliation = defer_reconciliation;

		foreach (lc, indexoids)
		{
			Oid				 indexoid = lfirst_oid(lc);
			Relation		 index_rel;
			TpReindexTarget *target;
			MemoryContext	 old_context;
			char			*lineage;
			bool			 lineage_backfilled = false;

			lineage = tp_ensure_index_compaction_lineage(
					indexoid, &lineage_backfilled);
			if (lineage == NULL)
				continue;
			index_rel = try_index_open(indexoid, AccessShareLock);
			if (index_rel == NULL)
			{
				pfree(lineage);
				continue;
			}
			if (!tp_is_background_index_relation(index_rel))
			{
				index_close(index_rel, AccessShareLock);
				pfree(lineage);
				continue;
			}

			old_context			  = MemoryContextSwitchTo(context);
			target				  = palloc0(sizeof(*target));
			target->heap_oid	  = index_rel->rd_index->indrelid;
			target->namespace_oid = RelationGetNamespace(index_rel);
			target->index_name = pstrdup(RelationGetRelationName(index_rel));
			target->index_oid  = indexoid;
			target->tablespace_oid = index_rel->rd_locator.spcOid;
			target->relfilenumber  = index_rel->rd_locator.relNumber;
			target->lineage		   = pstrdup(lineage);
			target->schedule =
					tp_compaction_job_schedule(indexoid, lineage_backfilled);
			target->lineage_backfilled = lineage_backfilled;
			state->targets			   = lappend(state->targets, target);
			MemoryContextSwitchTo(old_context);
			index_close(index_rel, AccessShareLock);
			pfree(lineage);
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(caller_context);
		MemoryContextDelete(context);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (state->targets == NIL)
	{
		MemoryContextDelete(context);
		return NULL;
	}

	tp_reindex_states = state;
	return state;
}

static void
tp_reindex_target_refresh_identity(
		TpReindexState *state, TpReindexTarget *target, Relation index_rel)
{
	MemoryContext old_context;

	old_context = MemoryContextSwitchTo(state->context);
	pfree(target->index_name);
	target->namespace_oid = RelationGetNamespace(index_rel);
	target->index_name	  = pstrdup(RelationGetRelationName(index_rel));
	MemoryContextSwitchTo(old_context);
}

static bool
tp_reindex_target_matches(const TpReindexTarget *target, Relation index_rel)
{
	const char *lineage = tp_index_compaction_lineage(index_rel);

	return index_rel->rd_index != NULL &&
		   index_rel->rd_index->indrelid == target->heap_oid &&
		   lineage != NULL && strcmp(lineage, target->lineage) == 0;
}

static bool
tp_reindex_target_live_original(
		TpReindexState	*state,
		TpReindexTarget *target,
		Oid				*indexoid,
		Oid				*tablespace_oid,
		RelFileNumber	*relfilenumber)
{
	Relation index_rel;

	*indexoid = InvalidOid;
	index_rel = try_relation_open(target->index_oid, AccessShareLock);
	if (index_rel == NULL)
		return false;

	if (index_rel->rd_rel->relkind != RELKIND_INDEX ||
		!tp_reindex_target_matches(target, index_rel))
	{
		relation_close(index_rel, AccessShareLock);
		return false;
	}

	/*
	 * A concurrent replacement invalidates the old OID before publishing the
	 * replacement under its original name.  Only that state permits fallback
	 * to a name lookup; a still-live index remains the authoritative target.
	 */
	if (!index_rel->rd_index->indisvalid || !index_rel->rd_index->indisready ||
		!index_rel->rd_index->indislive)
	{
		relation_close(index_rel, AccessShareLock);
		return false;
	}

	if (tp_is_background_physical_index_relation(index_rel))
	{
		tp_reindex_target_refresh_identity(state, target, index_rel);
		*indexoid		= target->index_oid;
		*tablespace_oid = index_rel->rd_locator.spcOid;
		*relfilenumber	= index_rel->rd_locator.relNumber;
	}
	relation_close(index_rel, AccessShareLock);
	return true;
}

static void
tp_reindex_collect_candidates(
		TpReindexState *state, List **candidates, List **indexoids)
{
	ListCell *lc;

	foreach (lc, state->targets)
	{
		TpReindexTarget	   *target = lfirst(lc);
		TpReindexCandidate *candidate;
		Relation			index_rel;
		Oid					indexoid;
		Oid					tablespace_oid;
		RelFileNumber		relfilenumber;

		if (!tp_reindex_target_live_original(
					state, target, &indexoid, &tablespace_oid, &relfilenumber))
		{
			indexoid = get_relname_relid(
					target->index_name, target->namespace_oid);
			if (!OidIsValid(indexoid))
				continue;

			index_rel = try_relation_open(indexoid, AccessShareLock);
			if (index_rel == NULL)
				continue;
			if (!tp_is_background_physical_index_relation(index_rel) ||
				!tp_reindex_target_matches(target, index_rel))
			{
				relation_close(index_rel, AccessShareLock);
				continue;
			}

			tp_reindex_target_refresh_identity(state, target, index_rel);
			relation_close(index_rel, AccessShareLock);
		}

		if (!OidIsValid(indexoid))
			continue;
		candidate			 = palloc(sizeof(*candidate));
		candidate->state	 = state;
		candidate->target	 = target;
		candidate->index_oid = indexoid;
		*candidates			 = lappend(*candidates, candidate);
		*indexoids			 = lappend_oid(*indexoids, indexoid);
	}
}

static void
tp_reconcile_reindex_states(bool include_deferred)
{
	List		   *active_states = NIL;
	List		   *candidates	  = NIL;
	List		   *indexoids	  = NIL;
	List		   *locked		  = NIL;
	ListCell	   *lc;
	TpReindexState *state;

	for (state = tp_reindex_states; state != NULL; state = state->previous)
	{
		if (state->reconciling ||
			(state->defer_reconciliation && !include_deferred))
			continue;
		state->reconciling = true;
		active_states	   = lappend(active_states, state);
	}

	PG_TRY();
	{
		foreach (lc, active_states)
			tp_reindex_collect_candidates(lfirst(lc), &candidates, &indexoids);

		locked = tp_prelock_compaction_indexes(indexoids);
		foreach (lc, candidates)
		{
			TpReindexCandidate *candidate = lfirst(lc);
			TpReindexTarget	   *target	  = candidate->target;
			Relation			index_rel;
			Oid					tablespace_oid;
			RelFileNumber		relfilenumber;

			index_rel = try_relation_open(candidate->index_oid, NoLock);
			if (index_rel == NULL)
				continue;
			if (!tp_is_background_physical_index_relation(index_rel) ||
				!tp_reindex_target_matches(target, index_rel))
			{
				relation_close(index_rel, NoLock);
				continue;
			}

			tp_reindex_target_refresh_identity(
					candidate->state, target, index_rel);
			tablespace_oid = index_rel->rd_locator.spcOid;
			relfilenumber  = index_rel->rd_locator.relNumber;
			relation_close(index_rel, NoLock);

			if (candidate->index_oid == target->index_oid &&
				tablespace_oid == target->tablespace_oid &&
				relfilenumber == target->relfilenumber &&
				!target->lineage_backfilled)
				continue;

			tp_compaction_job_activate_with_schedule(
					candidate->index_oid, target->schedule);
			target->index_oid		   = candidate->index_oid;
			target->tablespace_oid	   = tablespace_oid;
			target->relfilenumber	   = relfilenumber;
			target->lineage_backfilled = false;
		}
	}
	PG_FINALLY();
	{
		foreach (lc, active_states)
			((TpReindexState *)lfirst(lc))->reconciling = false;
		list_free(locked);
		list_free(indexoids);
		list_free_deep(candidates);
		list_free(active_states);
	}
	PG_END_TRY();
}

static void
tp_reindex_tracking_end(TpReindexState *state)
{
	MemoryContext context;

	if (state == NULL)
		return;

	Assert(tp_reindex_states == state);
	context			  = state->context;
	tp_reindex_states = state->previous;
	MemoryContextDelete(context);
}

static List *
tp_reindex_current_indexes(ReindexStmt *stmt)
{
	Oid relation_oid;

	relation_oid = RangeVarGetRelid(stmt->relation, AccessShareLock, false);
	if (stmt->kind == REINDEX_OBJECT_INDEX)
		return list_make1_oid(relation_oid);

	return tp_relation_indexes_locked(relation_oid, NoLock);
}

static bool
tp_vacuum_rewrites_storage(VacuumStmt *stmt)
{
	ListCell *lc;

	if (!stmt->is_vacuumcmd)
		return false;

	foreach (lc, stmt->options)
	{
		DefElem *option = lfirst_node(DefElem, lc);

		if (strcmp(option->defname, "full") == 0)
			return defGetBoolean(option);
	}
	return false;
}

static List *
tp_vacuum_rewrite_indexes(VacuumStmt *stmt)
{
	List	 *indexoids = NIL;
	ListCell *lc;

	if (stmt->rels == NIL)
		return tp_maintainable_indexes(
				tp_all_bm25_indexes(InvalidOid, false, NIL, false), true);

	foreach (lc, stmt->rels)
	{
		VacuumRelation *vacuum_rel	 = lfirst_node(VacuumRelation, lc);
		Oid				relation_oid = vacuum_rel->oid;
		List		   *relation_indexes;

		if (!OidIsValid(relation_oid) && vacuum_rel->relation != NULL)
			relation_oid = RangeVarGetRelid(
					vacuum_rel->relation, AccessShareLock, true);
		if (!OidIsValid(relation_oid))
			continue;
		if (!object_ownercheck(
					DatabaseRelationId, MyDatabaseId, GetUserId()) &&
			!tp_can_maintain_relation(relation_oid))
			continue;

		relation_indexes = tp_relation_tree_indexes_locked(
				relation_oid, AccessExclusiveLock);
		indexoids = list_concat_unique_oid(indexoids, relation_indexes);
	}
	return indexoids;
}

static List *
tp_cluster_rewrite_indexes(
#if PG_VERSION_NUM >= 190000
		RepackStmt *stmt,
#else
		ClusterStmt *stmt,
#endif
		bool *multi_transaction)
{
	RangeVar *relation;
	Oid		  relation_oid;

#if PG_VERSION_NUM >= 190000
	relation = stmt->relation == NULL ? NULL : stmt->relation->relation;
#else
	relation = stmt->relation;
#endif

	if (relation == NULL)
	{
		*multi_transaction = true;
		return tp_maintainable_indexes(
				tp_all_bm25_indexes(InvalidOid, false, NIL, false), true);
	}

	relation_oid = RangeVarGetRelidExtended(
			relation,
			AccessExclusiveLock,
			0,
			RangeVarCallbackMaintainsTable,
			NULL);
	*multi_transaction = get_rel_relkind(relation_oid) ==
						 RELKIND_PARTITIONED_TABLE;
	return tp_relation_tree_indexes_locked(relation_oid, NoLock);
}

static bool
tp_alter_index_sets_tablespace(AlterTableStmt *stmt)
{
	ListCell *lc;

	if (stmt->objtype != OBJECT_INDEX)
		return false;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		if (cmd->subtype == AT_SetTableSpace)
			return true;
	}
	return false;
}

static bool
tp_alter_table_may_rewrite(AlterTableStmt *stmt)
{
	ListCell *lc;

	if (stmt->objtype != OBJECT_TABLE && stmt->objtype != OBJECT_MATVIEW)
		return false;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		switch (cmd->subtype)
		{
		case AT_AddColumn:
		case AT_AlterColumnType:
		case AT_SetExpression:
		case AT_SetLogged:
		case AT_SetUnLogged:
		case AT_SetAccessMethod:
			return true;
		default:
			break;
		}
	}
	return false;
}

static bool
tp_alter_table_attaches_partition(AlterTableStmt *stmt)
{
	ListCell *lc;

	if (stmt->objtype != OBJECT_TABLE && stmt->objtype != OBJECT_INDEX)
		return false;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		if (cmd->subtype == AT_AttachPartition)
			return true;
	}
	return false;
}

static void
tp_truncate_check_relation(Oid relation_oid, const char *relation_name)
{
	HeapTuple	  tuple;
	Form_pg_class relation_form;
	AclResult	  aclresult;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relation_oid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relation_oid);
	relation_form = (Form_pg_class)GETSTRUCT(tuple);

	if (relation_form->relkind != RELKIND_RELATION &&
		relation_form->relkind != RELKIND_PARTITIONED_TABLE &&
		relation_form->relkind != RELKIND_FOREIGN_TABLE)
	{
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table", relation_name)));
	}

	aclresult = pg_class_aclcheck(relation_oid, GetUserId(), ACL_TRUNCATE);
	if (aclresult != ACLCHECK_OK)
	{
		ObjectType object_type = get_relkind_objtype(relation_form->relkind);

		ReleaseSysCache(tuple);
		aclcheck_error(aclresult, object_type, relation_name);
	}
	ReleaseSysCache(tuple);
}

static void
tp_truncate_lookup_callback(
		const RangeVar *relation,
		Oid				relid,
		Oid old_relid	pg_attribute_unused(),
		void *arg		pg_attribute_unused())
{
	if (OidIsValid(relid))
		tp_truncate_check_relation(relid, relation->relname);
}

static List *
tp_truncate_rewrite_indexes(TruncateStmt *stmt)
{
	List	 *indexoids		= NIL;
	List	 *relation_oids = NIL;
	ListCell *lc;

	foreach (lc, stmt->relations)
	{
		RangeVar *relation = lfirst_node(RangeVar, lc);
		Oid		  relation_oid;

		relation_oid = RangeVarGetRelidExtended(
				relation,
				AccessExclusiveLock,
				0,
				tp_truncate_lookup_callback,
				NULL);
		relation_oids = list_append_unique_oid(relation_oids, relation_oid);
		if (relation->inh)
		{
			List *children = find_all_inheritors(
					relation_oid, AccessExclusiveLock, NULL);

			relation_oids = list_concat_unique_oid(relation_oids, children);
		}
	}

	if (stmt->behavior == DROP_CASCADE)
	{
		for (;;)
		{
			List *new_relation_oids = heap_truncate_find_FKs(relation_oids);

			if (new_relation_oids == NIL)
				break;
			foreach (lc, new_relation_oids)
			{
				Oid	  relation_oid = lfirst_oid(lc);
				char *relation_name;

				LockRelationOid(relation_oid, AccessExclusiveLock);
				relation_name = get_rel_name(relation_oid);
				if (relation_name == NULL)
					continue;
				tp_truncate_check_relation(relation_oid, relation_name);
				pfree(relation_name);
				relation_oids =
						list_append_unique_oid(relation_oids, relation_oid);
			}
			list_free(new_relation_oids);
		}
	}

	foreach (lc, relation_oids)
	{
		List *relation_indexes =
				tp_relation_indexes_locked(lfirst_oid(lc), NoLock);

		indexoids = list_concat_unique_oid(indexoids, relation_indexes);
	}
	list_free(relation_oids);
	return indexoids;
}

static List *
tp_refresh_matview_rewrite_indexes(RefreshMatViewStmt *stmt)
{
	LOCKMODE lockmode = stmt->concurrent ? ExclusiveLock : AccessExclusiveLock;
	Oid		 relation_oid;

	relation_oid = RangeVarGetRelidExtended(
			stmt->relation, lockmode, 0, RangeVarCallbackMaintainsTable, NULL);
	return tp_relation_indexes_locked(relation_oid, NoLock);
}

static void
tp_process_tracked_rewrite(
		PlannedStmt			 *pstmt,
		const char			 *queryString,
		bool				  readOnlyTree,
		ProcessUtilityContext context,
		ParamListInfo		  params,
		QueryEnvironment	 *queryEnv,
		DestReceiver		 *dest,
		QueryCompletion		 *qc,
		List				 *indexoids,
		bool				  nowait)
{
	TpReindexState *rewrite_state = NULL;
	List		   *candidates;
	List		   *locked;

	candidates = tp_physical_bm25_indexes(indexoids, true);
	locked	   = tp_prelock_compaction_indexes_nowait(candidates, nowait);
	list_free(candidates);
	rewrite_state = tp_reindex_tracking_begin(locked, false);
	list_free(locked);
	list_free(indexoids);

	PG_TRY();
	{
		if (prev_process_utility_hook)
			prev_process_utility_hook(
					pstmt,
					queryString,
					readOnlyTree,
					context,
					params,
					queryEnv,
					dest,
					qc);
		else
			standard_ProcessUtility(
					pstmt,
					queryString,
					readOnlyTree,
					context,
					params,
					queryEnv,
					dest,
					qc);

		tp_reconcile_reindex_states(true);
	}
	PG_FINALLY();
	{
		tp_reindex_tracking_end(rewrite_state);
	}
	PG_END_TRY();
}

static RoleSpec *
tp_alter_new_owner(AlterTableStmt *stmt)
{
	ListCell *lc;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		if (cmd->subtype != AT_ChangeOwner)
			continue;

		return cmd->newowner;
	}

	return NULL;
}

/*
 * ProcessUtility hook - detect CREATE INDEX USING bm25 and wrap
 * with build progress tracking. This collapses per-partition
 * NOTICEs into a single summary for partitioned tables.
 */
static void
tp_process_utility(
		PlannedStmt			 *pstmt,
		const char			 *queryString,
		bool				  readOnlyTree,
		ProcessUtilityContext context,
		ParamListInfo		  params,
		QueryEnvironment	 *queryEnv,
		DestReceiver		 *dest,
		QueryCompletion		 *qc)
{
	tp_process_utility_depth++;
	PG_TRY();
	{
		tp_process_utility_impl(
				pstmt,
				queryString,
				readOnlyTree,
				context,
				params,
				queryEnv,
				dest,
				qc);
	}
	PG_FINALLY();
	{
		tp_process_utility_depth--;
	}
	PG_END_TRY();
}

static void
tp_process_utility_impl(
		PlannedStmt			 *pstmt,
		const char			 *queryString,
		bool				  readOnlyTree,
		ProcessUtilityContext context,
		ParamListInfo		  params,
		QueryEnvironment	 *queryEnv,
		DestReceiver		 *dest,
		QueryCompletion		 *qc)
{
	Node *parsetree = pstmt->utilityStmt;

	if (IsA(parsetree, VacuumStmt) &&
		tp_vacuum_rewrites_storage(castNode(VacuumStmt, parsetree)))
	{
		PreventInTransactionBlock(
				context == PROCESS_UTILITY_TOPLEVEL, "VACUUM");
		tp_process_tracked_rewrite(
				pstmt,
				queryString,
				readOnlyTree,
				context,
				params,
				queryEnv,
				dest,
				qc,
				tp_vacuum_rewrite_indexes(castNode(VacuumStmt, parsetree)),
				false);
		return;
	}

#if PG_VERSION_NUM >= 190000
	if (IsA(parsetree, RepackStmt) &&
		castNode(RepackStmt, parsetree)->command == REPACK_COMMAND_CLUSTER)
#else
	if (IsA(parsetree, ClusterStmt))
#endif
	{
		bool multi_transaction;
#if PG_VERSION_NUM >= 190000
		List *indexoids = tp_cluster_rewrite_indexes(
				castNode(RepackStmt, parsetree), &multi_transaction);
#else
		List *indexoids = tp_cluster_rewrite_indexes(
				castNode(ClusterStmt, parsetree), &multi_transaction);
#endif

		if (multi_transaction)
			PreventInTransactionBlock(
					context == PROCESS_UTILITY_TOPLEVEL, "CLUSTER");
		tp_process_tracked_rewrite(
				pstmt,
				queryString,
				readOnlyTree,
				context,
				params,
				queryEnv,
				dest,
				qc,
				indexoids,
				false);
		return;
	}

	if (IsA(parsetree, TruncateStmt))
	{
		tp_process_tracked_rewrite(
				pstmt,
				queryString,
				readOnlyTree,
				context,
				params,
				queryEnv,
				dest,
				qc,
				tp_truncate_rewrite_indexes(castNode(TruncateStmt, parsetree)),
				false);
		return;
	}

	if (IsA(parsetree, RefreshMatViewStmt))
	{
		tp_process_tracked_rewrite(
				pstmt,
				queryString,
				readOnlyTree,
				context,
				params,
				queryEnv,
				dest,
				qc,
				tp_refresh_matview_rewrite_indexes(
						castNode(RefreshMatViewStmt, parsetree)),
				false);
		return;
	}

	if (IsA(parsetree, AlterTableMoveAllStmt))
	{
		AlterTableMoveAllStmt *stmt =
				castNode(AlterTableMoveAllStmt, parsetree);

		if (stmt->objtype == OBJECT_INDEX)
		{
			Oid tablespace_oid =
					get_tablespace_oid(stmt->orig_tablespacename, false);
			List *owner_oids			= tp_role_oids(stmt->roles);
			bool  require_current_owner = stmt->roles == NIL && !superuser();

			tp_process_tracked_rewrite(
					pstmt,
					queryString,
					readOnlyTree,
					context,
					params,
					queryEnv,
					dest,
					qc,
					tp_all_bm25_indexes(
							tp_catalog_tablespace_oid(tablespace_oid),
							true,
							owner_oids,
							require_current_owner),
					stmt->nowait);
			list_free(owner_oids);
			return;
		}
	}

	if (IsA(parsetree, ReassignOwnedStmt))
	{
		List *indexoids = tp_reassign_owned_indexes(
				castNode(ReassignOwnedStmt, parsetree));
		List *candidates = tp_physical_bm25_indexes(indexoids, true);
		List *locked;
		List *owner_targets;

		tp_prelock_owner_change_heaps(candidates);
		locked		  = tp_prelock_compaction_indexes(candidates);
		owner_targets = tp_capture_owner_change_targets(locked);
		list_free(candidates);
		list_free(indexoids);
		PG_TRY();
		{
			if (prev_process_utility_hook)
				prev_process_utility_hook(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);
			else
				standard_ProcessUtility(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);

			tp_activate_owner_change_targets(owner_targets);
		}
		PG_FINALLY();
		{
			tp_free_owner_change_targets(owner_targets);
			list_free(locked);
		}
		PG_END_TRY();
		return;
	}

	if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *)parsetree;
		RoleSpec	   *new_owner;

		tp_reject_user_lineage_alter(stmt);
		if (tp_alter_index_sets_tablespace(stmt))
		{
			Oid indexoid = RangeVarGetRelidExtended(
					stmt->relation,
					AlterTableGetLockLevel(stmt->cmds),
					stmt->missing_ok ? RVR_MISSING_OK : 0,
					RangeVarCallbackOwnsRelation,
					NULL);

			if (OidIsValid(indexoid))
				tp_process_tracked_rewrite(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc,
						tp_index_tree_locked(indexoid, AccessShareLock),
						false);
			else if (prev_process_utility_hook)
				prev_process_utility_hook(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);
			else
				standard_ProcessUtility(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);
			return;
		}

		if (tp_alter_table_attaches_partition(stmt))
		{
			TpCreateIndexState *create_state = NULL;
			LOCKMODE			lockmode = AlterTableGetLockLevel(stmt->cmds);
			Oid					relation_oid;

			relation_oid = RangeVarGetRelidExtended(
					stmt->relation,
					lockmode,
					stmt->missing_ok ? RVR_MISSING_OK : 0,
					RangeVarCallbackOwnsRelation,
					NULL);
			if (!OidIsValid(relation_oid))
			{
				if (prev_process_utility_hook)
					prev_process_utility_hook(
							pstmt,
							queryString,
							readOnlyTree,
							context,
							params,
							queryEnv,
							dest,
							qc);
				else
					standard_ProcessUtility(
							pstmt,
							queryString,
							readOnlyTree,
							context,
							params,
							queryEnv,
							dest,
							qc);
				return;
			}

			PG_TRY();
			{
				if (stmt->objtype == OBJECT_TABLE)
					create_state = tp_create_index_tracking_begin(
							relation_oid);
				if (prev_process_utility_hook)
					prev_process_utility_hook(
							pstmt,
							queryString,
							readOnlyTree,
							context,
							params,
							queryEnv,
							dest,
							qc);
				else
					standard_ProcessUtility(
							pstmt,
							queryString,
							readOnlyTree,
							context,
							params,
							queryEnv,
							dest,
							qc);

				if (stmt->objtype == OBJECT_TABLE)
					tp_reconcile_partition_index_options(relation_oid);
				else
					tp_reconcile_attached_index_options(relation_oid);
			}
			PG_FINALLY();
			{
				tp_create_index_tracking_end(create_state);
			}
			PG_END_TRY();
			return;
		}

		if (tp_alter_table_may_rewrite(stmt))
		{
			LOCKMODE lockmode = AlterTableGetLockLevel(stmt->cmds);
			Oid		 relation_oid;

			relation_oid = RangeVarGetRelidExtended(
					stmt->relation,
					lockmode,
					stmt->missing_ok ? RVR_MISSING_OK : 0,
					RangeVarCallbackOwnsRelation,
					NULL);
			if (OidIsValid(relation_oid))
				tp_process_tracked_rewrite(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc,
						tp_relation_tree_indexes_locked(relation_oid, NoLock),
						false);
			else if (prev_process_utility_hook)
				prev_process_utility_hook(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);
			else
				standard_ProcessUtility(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);
			return;
		}

		new_owner = tp_alter_new_owner(stmt);
		if (new_owner != NULL &&
			(stmt->objtype == OBJECT_TABLE || stmt->objtype == OBJECT_MATVIEW))
		{
			Oid	  relation_oid;
			List *owner_targets = NIL;

			relation_oid = RangeVarGetRelidExtended(
					stmt->relation,
					AlterTableGetLockLevel(stmt->cmds),
					stmt->missing_ok ? RVR_MISSING_OK : 0,
					RangeVarCallbackOwnsRelation,
					NULL);
			if (OidIsValid(relation_oid) &&
				object_ownercheck(
						RelationRelationId, relation_oid, GetUserId()))
			{
				List *indexoids =
						tp_relation_indexes_locked(relation_oid, NoLock);
				List *candidates = tp_physical_bm25_indexes(indexoids, true);
				List *locked	 = tp_prelock_compaction_indexes(candidates);

				owner_targets = tp_capture_owner_change_targets(locked);
				list_free(locked);
				list_free(candidates);
				list_free(indexoids);
			}

			PG_TRY();
			{
				if (prev_process_utility_hook)
					prev_process_utility_hook(
							pstmt,
							queryString,
							readOnlyTree,
							context,
							params,
							queryEnv,
							dest,
							qc);
				else
					standard_ProcessUtility(
							pstmt,
							queryString,
							readOnlyTree,
							context,
							params,
							queryEnv,
							dest,
							qc);

				tp_activate_owner_change_targets(owner_targets);
			}
			PG_FINALLY();
			{
				tp_free_owner_change_targets(owner_targets);
			}
			PG_END_TRY();
			return;
		}

		if (tp_alter_index_refreshes_background(stmt))
		{
			Oid indexoid;

			if (prev_process_utility_hook)
				prev_process_utility_hook(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);
			else
				standard_ProcessUtility(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);

			indexoid = RangeVarGetRelid(
					stmt->relation, AccessShareLock, stmt->missing_ok);
			if (!OidIsValid(indexoid))
				return;

			if (tp_is_background_physical_index(indexoid))
				tp_compaction_job_activate(indexoid, true);
			return;
		}
	}

	if (IsA(parsetree, ReindexStmt))
	{
		ReindexStmt	   *stmt = (ReindexStmt *)parsetree;
		List		   *indexoids;
		bool			tracks_commits;
		TpReindexState *reindex_state = NULL;

		if (stmt->kind == REINDEX_OBJECT_SCHEMA ||
			stmt->kind == REINDEX_OBJECT_DATABASE)
			PreventInTransactionBlock(
					context == PROCESS_UTILITY_TOPLEVEL, "REINDEX");

		indexoids = tp_reindex_initial_indexes(stmt, &tracks_commits);

		PG_TRY();
		{
			if (tracks_commits)
			{
				List *candidates = tp_physical_bm25_indexes(indexoids, true);
				List *locked	 = tp_prelock_compaction_indexes(candidates);

				list_free(candidates);
				reindex_state = tp_reindex_tracking_begin(
						locked,
						stmt->kind == REINDEX_OBJECT_SCHEMA ||
								stmt->kind == REINDEX_OBJECT_DATABASE);
				list_free(locked);
			}
			list_free(indexoids);

			if (prev_process_utility_hook)
				prev_process_utility_hook(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);
			else
				standard_ProcessUtility(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);

			if (tracks_commits)
				tp_reconcile_reindex_states(true);
			else if (
					stmt->kind == REINDEX_OBJECT_INDEX ||
					stmt->kind == REINDEX_OBJECT_TABLE)
			{
				indexoids = tp_reindex_current_indexes(stmt);
				tp_activate_background_indexes(indexoids, true);
				list_free(indexoids);
			}
		}
		PG_FINALLY();
		{
			tp_reindex_tracking_end(reindex_state);
		}
		PG_END_TRY();
		return;
	}

	if (IsA(parsetree, CreateStmt))
	{
		TpCreateIndexState *create_state = NULL;

		PG_TRY();
		{
			create_state = tp_create_index_tracking_begin(InvalidOid);
			if (prev_process_utility_hook)
				prev_process_utility_hook(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);
			else
				standard_ProcessUtility(
						pstmt,
						queryString,
						readOnlyTree,
						context,
						params,
						queryEnv,
						dest,
						qc);

			tp_activate_background_indexes(
					create_state->created_indexes, true);
		}
		PG_FINALLY();
		{
			tp_create_index_tracking_end(create_state);
		}
		PG_END_TRY();
		return;
	}

	if (IsA(parsetree, IndexStmt))
	{
		IndexStmt *stmt = (IndexStmt *)parsetree;

		if (stmt->accessMethod && strcmp(stmt->accessMethod, "bm25") == 0)
		{
			Oid					heapoid;
			Relation			heap_rel;
			TpCreateIndexState *create_state = NULL;
			const char		   *compaction;

			if (readOnlyTree)
			{
				pstmt		 = copyObject(pstmt);
				parsetree	 = pstmt->utilityStmt;
				stmt		 = castNode(IndexStmt, parsetree);
				readOnlyTree = false;
			}

			heapoid = RangeVarGetRelidExtended(
					stmt->relation,
					stmt->concurrent ? ShareUpdateExclusiveLock : ShareLock,
					0,
					RangeVarCallbackOwnsRelation,
					NULL);
			heap_rel   = relation_open(heapoid, NoLock);
			compaction = tp_index_stmt_option(stmt, "compaction");
			tp_index_stmt_validate_supplied_lineage(
					stmt, heapoid, heap_rel->rd_rel->relowner);
			if (compaction != NULL && strcmp(compaction, "background") == 0)
				tp_index_stmt_ensure_lineage(
						stmt, heapoid, heap_rel->rd_rel->relowner);

			/*
			 * CIC commits its catalog shell before returning to this hook.
			 * Once PostgreSQL's ownership check is known to succeed, validate
			 * deterministic owner admission and workflow construction before
			 * those irreversible phases.
			 */
			if (tp_background_cic_needs_preflight(stmt, heap_rel) &&
				object_ownercheck(RelationRelationId, heapoid, GetUserId()))
			{
				const char *schedule =
						tp_index_stmt_option(stmt, "compaction_schedule");

				if (schedule == NULL)
					schedule = tp_background_compaction_schedule;
				tp_compaction_job_preflight(
						heap_rel->rd_rel->relowner, schedule);
			}

			relation_close(heap_rel, NoLock);

			PG_TRY();
			{
				create_state = tp_create_index_tracking_begin(heapoid);
				tp_build_progress_begin();

				if (prev_process_utility_hook)
					prev_process_utility_hook(
							pstmt,
							queryString,
							readOnlyTree,
							context,
							params,
							queryEnv,
							dest,
							qc);
				else
					standard_ProcessUtility(
							pstmt,
							queryString,
							readOnlyTree,
							context,
							params,
							queryEnv,
							dest,
							qc);

				tp_build_progress_end();

				if (compaction != NULL &&
					strcmp(compaction, "background") == 0)
					tp_reconcile_created_background_indexes(
							create_state->created_indexes,
							create_state->heap_oid,
							tp_index_stmt_option(stmt, "compaction_schedule"),
							tp_index_stmt_option(stmt, "compaction_lineage"));
				else
					tp_activate_background_indexes(
							create_state->created_indexes, true);
			}
			PG_FINALLY();
			{
				tp_create_index_tracking_end(create_state);
			}
			PG_END_TRY();
			return;
		}
	}

	/* Not a bm25 CREATE INDEX - pass through */
	if (prev_process_utility_hook)
		prev_process_utility_hook(
				pstmt,
				queryString,
				readOnlyTree,
				context,
				params,
				queryEnv,
				dest,
				qc);
	else
		standard_ProcessUtility(
				pstmt,
				queryString,
				readOnlyTree,
				context,
				params,
				queryEnv,
				dest,
				qc);
}

/*
 * Deprecated stub for legacy upgrade compatibility.
 *
 * pg_textsearch--1.0.0--1.1.0.sql ships with a CREATE FUNCTION
 * bm25_memory_usage() that binds to this C symbol.  The SRF and
 * its underlying soft-limit infrastructure were removed in
 * 1.3.0 (issue #374), and the matching SQL function is
 * DROPped by pg_textsearch--1.2.0--1.3.0.sql, but during an
 * ALTER EXTENSION UPDATE chain that walks 1.0.0 -> 1.3.0,
 * the CREATE in 1.0.0--1.1.0 has to find this symbol before the
 * DROP can run.  The stub returns NULL.
 */
PG_FUNCTION_INFO_V1(tp_memory_usage);

Datum
tp_memory_usage(PG_FUNCTION_ARGS)
{
	PG_RETURN_NULL();
}
