/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * mod.c - Extension initialization and GUC registration
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/relation.h>
#include <access/reloptions.h>
#include <access/skey.h>
#include <access/table.h>
#include <access/xact.h>
#include <catalog/dependency.h>
#include <catalog/index.h>
#include <catalog/indexing.h>
#include <catalog/namespace.h>
#include <catalog/objectaccess.h>
#include <catalog/pg_class_d.h>
#include <catalog/pg_extension_d.h>
#include <catalog/pg_inherits.h>
#include <catalog/pg_inherits_d.h>
#include <commands/extension.h>
#include <commands/defrem.h>
#include <commands/tablecmds.h>
#include <fmgr.h>
#include <limits.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/parsenodes.h>
#include <nodes/pg_list.h>
#include <pg_config.h>
#include <storage/ipc.h>
#include <storage/lmgr.h>
#include <storage/lock.h>
#include <storage/shmem.h>
#include <tcop/utility.h>
#include <utils/acl.h>
#include <utils/fmgroids.h>
#include <utils/guc.h>
#include <utils/inval.h>
#include <utils/lsyscache.h>
#include <utils/snapmgr.h>
#include <utils/relcache.h>

#include "access/am.h"
#include "access/rls.h"
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

/* Allow BM25 indexes on relations protected by row-level security. */
bool tp_allow_rls = true;

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

typedef struct TpProcessUtilityContext
{
	struct TpProcessUtilityContext *previous;
	bool							extension_lifecycle;
	bool							track_index_build;
	bool							check_rls_enable;
	bool							check_hierarchy_change;
	bool							track_relation_create;
	bool							serialize_rls_ddl;
	bool							retain_rls_ddl_lock;
	bool							allow_rls;
	bool							rls_ddl_lock_acquired;
	LOCKMODE						rls_ddl_lock_mode;
	bool							build_progress_started;
	Oid								rls_ddl_lock_object;
	List						   *altered_relids;
	List						   *hierarchy_relids;
} TpProcessUtilityContext;

static TpProcessUtilityContext *current_utility_context = NULL;

bool
tp_rls_allowed_for_current_utility(void)
{
	if (current_utility_context != NULL)
		return current_utility_context->allow_rls;

	return tp_allow_rls;
}

void
tp_rls_note_bm25_build(void)
{
	if (current_utility_context != NULL)
		current_utility_context->retain_rls_ddl_lock = true;
}

/*
 * Commands allowed to create an RLS/BM25 combination take a shared lock;
 * commands enforcing the restriction and extension lifecycle commands take
 * an exclusive lock.  The stable lock serializes across extension OID
 * replacement, while the current extension-object lock preserves core lock
 * ordering.  Session ownership survives internal commits; transaction
 * ownership covers completed protected changes through commit.
 *
 * Nested acquisition without an inherited lock, or an exclusive request while
 * this backend holds only a shared lock, must not wait.  Either case can
 * invert lock order with an outer command or deadlock with another backend
 * upgrading the same lock.
 */
static bool
acquire_rls_ddl_lock(
		LOCKMODE lockmode,
		bool	 nested,
		bool	 extension_lifecycle,
		Oid		*extension_oid_out)
{
	LOCKTAG			  stable_tag;
	LOCKTAG			  extension_tag;
	LockAcquireResult result;
	Oid				  extension_oid = InvalidOid;
	bool			  dont_wait;

	if (!extension_lifecycle &&
		!OidIsValid(get_extension_oid("pg_textsearch", true)))
		return false;

	SET_LOCKTAG_OBJECT(
			stable_tag, MyDatabaseId, ExtensionRelationId, InvalidOid, 0);
	dont_wait = (lockmode == ExclusiveLock &&
				 LockHeldByMe(&stable_tag, ShareLock, false) &&
				 !LockHeldByMe(&stable_tag, ExclusiveLock, true)) ||
				(nested && !LockHeldByMe(&stable_tag, lockmode, true));
	result = LockAcquire(&stable_tag, lockmode, true, dont_wait);
	if (result == LOCKACQUIRE_NOT_AVAIL)
		ereport(ERROR,
				(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
				 errmsg("could not acquire the pg_textsearch RLS DDL lock"),
				 errdetail(
						 "Protected DDL was invoked from a nested utility "
						 "command whose outer command does not hold the "
						 "pg_textsearch DDL lock."),
				 errhint("Retry the outer command.")));

	extension_oid = get_extension_oid("pg_textsearch", true);
	if (OidIsValid(extension_oid))
	{
		SET_LOCKTAG_OBJECT(
				extension_tag,
				MyDatabaseId,
				ExtensionRelationId,
				extension_oid,
				0);
		result = LockAcquire(&extension_tag, lockmode, true, dont_wait);
		if (result == LOCKACQUIRE_NOT_AVAIL)
		{
			(void)LockRelease(&stable_tag, lockmode, true);
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not acquire the pg_textsearch RLS DDL "
							"lock"),
					 errdetail(
							 "Protected DDL was invoked from a nested utility "
							 "command whose outer command does not hold the "
							 "pg_textsearch DDL lock."),
					 errhint("Retry the outer command.")));
		}
	}

	*extension_oid_out = extension_oid;
	return true;
}

static void
release_rls_ddl_lock(
		Oid extension_oid, LOCKMODE lockmode, bool keep_transaction_lock)
{
	LOCKTAG stable_tag;
	LOCKTAG extension_tag;

	SET_LOCKTAG_OBJECT(
			stable_tag, MyDatabaseId, ExtensionRelationId, InvalidOid, 0);
	if (OidIsValid(extension_oid))
		SET_LOCKTAG_OBJECT(
				extension_tag,
				MyDatabaseId,
				ExtensionRelationId,
				extension_oid,
				0);

	if (keep_transaction_lock)
	{
		(void)LockAcquire(&stable_tag, lockmode, false, false);
		if (OidIsValid(extension_oid))
			(void)LockAcquire(&extension_tag, lockmode, false, false);
	}

	if (OidIsValid(extension_oid))
		(void)LockRelease(&extension_tag, lockmode, true);
	(void)LockRelease(&stable_tag, lockmode, true);
}

typedef struct TpReindexTarget
{
	Oid			  heap_oid;
	Oid			  namespace_oid;
	char		 *index_name;
	Oid			  index_oid;
	Oid			  tablespace_oid;
	RelFileNumber relfilenumber;
	char		 *lineage;
} TpReindexTarget;

typedef struct TpReindexState
{
	MemoryContext		   context;
	List				  *targets;
	bool				   reconciling;
	struct TpReindexState *previous;
} TpReindexState;

/*
 * Concurrent and partitioned REINDEX commit inside ProcessUtility.  Keep a
 * stack of invocation-owned targets across those commits so each replacement
 * can be reconciled in the transaction that publishes it.
 */
static TpReindexState *tp_reindex_states = NULL;

/* Shared memory size calculation */
static void tp_shmem_request(void);

/* Shared memory startup hook */
static void tp_shmem_startup(void);

/* Object access hook for catalog-object validation and DROP INDEX cleanup */
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

/* ProcessUtility hook for nestable DDL validation and build tracking */
static void tp_process_utility(
		PlannedStmt			 *pstmt,
		const char			 *queryString,
		bool				  readOnlyTree,
		ProcessUtilityContext context,
		ParamListInfo		  params,
		QueryEnvironment	 *queryEnv,
		DestReceiver		 *dest,
		QueryCompletion		 *qc);
static void tp_reconcile_reindex_states(void);

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
			"pg_textsearch.allow_rls",
			"Allow BM25 indexes on row-level security tables.",
			"When disabled, BM25 indexes cannot be created or rebuilt on "
			"RLS-protected relations, and RLS cannot be enabled on relations "
			"that have BM25 indexes.",
			&tp_allow_rls,
			true,
			PGC_SUSET,
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

/*
 * Object access hook - enforce catalog-object RLS checks and handle drops.
 */
static void
tp_object_access(
		ObjectAccessType access,
		Oid				 classId,
		Oid				 objectId,
		int				 subId,
		void			*arg)
{
	/* Call previous hook if exists */
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	if (access == OAT_POST_CREATE && classId == RelationRelationId &&
		subId == 0)
	{
		bool is_bm25 = tp_check_bm25_index_create_allowed(objectId);

		if (is_bm25 && current_utility_context != NULL &&
			current_utility_context->track_index_build &&
			!current_utility_context->build_progress_started)
		{
			tp_build_progress_begin();
			current_utility_context->build_progress_started = true;
		}

		if (current_utility_context != NULL &&
			(is_bm25 || current_utility_context->track_relation_create))
			current_utility_context->retain_rls_ddl_lock = true;
	}

	if (access == OAT_POST_ALTER && current_utility_context != NULL &&
		subId == 0)
	{
		if (classId == RelationRelationId &&
			current_utility_context->check_rls_enable &&
			!list_member_oid(
					current_utility_context->altered_relids, objectId))
		{
			current_utility_context->altered_relids = lappend_oid(
					current_utility_context->altered_relids, objectId);
			current_utility_context->retain_rls_ddl_lock = true;
		}
		else if (
				classId == InheritsRelationId &&
				current_utility_context->check_hierarchy_change &&
				!list_member_oid(
						current_utility_context->hierarchy_relids, objectId))
		{
			current_utility_context->hierarchy_relids = lappend_oid(
					current_utility_context->hierarchy_relids, objectId);
			current_utility_context->retain_rls_ddl_lock = true;
		}
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
		tp_reconcile_reindex_states();

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
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background compaction lineage is already in use")));

	tp_lock_compaction_lineage(lineage);
	if (tp_compaction_lineage_in_use(lineage, heap_oid, owner_oid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("background compaction lineage is already in use")));
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
	Oid		 indexoid;
	Relation index_rel;
	bool	 is_bm25;

	if (!tp_alter_index_mentions_lineage(stmt))
		return;

	indexoid = RangeVarGetRelid(stmt->relation, NoLock, stmt->missing_ok);
	if (!OidIsValid(indexoid) ||
		!object_ownercheck(RelationRelationId, indexoid, GetUserId()))
		return;

	index_rel = try_index_open(indexoid, AccessShareLock);
	if (index_rel == NULL)
		return;
	is_bm25 = index_rel->rd_indam != NULL &&
			  index_rel->rd_indam->ambuild == tp_build;
	index_close(index_rel, AccessShareLock);

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
tp_is_background_physical_index_relation(Relation index_rel)
{
	return tp_is_background_index_relation(index_rel) &&
		   index_rel->rd_index->indisvalid &&
		   index_rel->rd_index->indisready && index_rel->rd_index->indislive;
}

static bool
tp_is_background_physical_index(Oid indexoid)
{
	Relation index_rel;
	bool	 background;

	index_rel = try_index_open(indexoid, AccessShareLock);
	if (index_rel == NULL)
		return false;

	background = tp_is_background_physical_index_relation(index_rel);
	index_close(index_rel, AccessShareLock);
	return background;
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
	if (concurrently)
	{
		ListCell *lc;

		foreach (lc, indexoids)
			LockRelationOid(lfirst_oid(lc), ShareUpdateExclusiveLock);
	}
	return indexoids;
}

static void
tp_activate_background_indexes(List *indexoids, bool refresh_default)
{
	ListCell *lc;

	foreach (lc, indexoids)
	{
		Oid indexoid = lfirst_oid(lc);

		if (tp_is_background_physical_index(indexoid))
			tp_compaction_job_activate(indexoid, refresh_default);
	}
}

static TpReindexState *
tp_reindex_tracking_begin(List *indexoids)
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

		foreach (lc, indexoids)
		{
			Oid				 indexoid = lfirst_oid(lc);
			Relation		 index_rel;
			TpReindexTarget *target;
			MemoryContext	 old_context;
			char			*lineage;

			lineage = tp_ensure_index_compaction_lineage(indexoid, NULL);
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
			state->targets		   = lappend(state->targets, target);
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
tp_reconcile_reindex_state(TpReindexState *state)
{
	ListCell *lc;

	if (state->reconciling)
		return;

	state->reconciling = true;
	PG_TRY();
	{
		foreach (lc, state->targets)
		{
			TpReindexTarget *target = lfirst(lc);
			Relation		 index_rel;
			Oid				 indexoid;
			Oid				 tablespace_oid;
			RelFileNumber	 relfilenumber;

			if (!tp_reindex_target_live_original(
						state,
						target,
						&indexoid,
						&tablespace_oid,
						&relfilenumber))
			{
				indexoid = get_relname_relid(
						target->index_name, target->namespace_oid);
				if (!OidIsValid(indexoid))
					continue;

				index_rel = try_index_open(indexoid, AccessShareLock);
				if (index_rel == NULL)
					continue;
				if (!tp_is_background_physical_index_relation(index_rel) ||
					!tp_reindex_target_matches(target, index_rel))
				{
					index_close(index_rel, AccessShareLock);
					continue;
				}

				tp_reindex_target_refresh_identity(state, target, index_rel);
				tablespace_oid = index_rel->rd_locator.spcOid;
				relfilenumber  = index_rel->rd_locator.relNumber;
				index_close(index_rel, AccessShareLock);
			}

			if (!OidIsValid(indexoid))
				continue;
			if (indexoid == target->index_oid &&
				tablespace_oid == target->tablespace_oid &&
				relfilenumber == target->relfilenumber)
				continue;

			tp_compaction_job_activate(indexoid, true);
			target->index_oid	   = indexoid;
			target->tablespace_oid = tablespace_oid;
			target->relfilenumber  = relfilenumber;
		}
	}
	PG_FINALLY();
	{
		state->reconciling = false;
	}
	PG_END_TRY();
}

static void
tp_reconcile_reindex_states(void)
{
	TpReindexState *state;

	for (state = tp_reindex_states; state != NULL; state = state->previous)
		tp_reconcile_reindex_state(state);
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
initialize_utility_context(
		TpProcessUtilityContext *utility_context, Node *stmt)
{
	memset(utility_context, 0, sizeof(*utility_context));
	utility_context->previous  = current_utility_context;
	utility_context->allow_rls = tp_allow_rls;

	if (IsA(stmt, CreateExtensionStmt))
	{
		CreateExtensionStmt *create_stmt = castNode(CreateExtensionStmt, stmt);

		utility_context->extension_lifecycle = strcmp(create_stmt->extname,
													  "pg_textsearch") == 0;
	}
	else if (IsA(stmt, AlterExtensionStmt))
	{
		AlterExtensionStmt *alter_stmt = castNode(AlterExtensionStmt, stmt);

		utility_context->extension_lifecycle = strcmp(alter_stmt->extname,
													  "pg_textsearch") == 0;
	}
	else if (IsA(stmt, AlterExtensionContentsStmt))
	{
		AlterExtensionContentsStmt *alter_stmt =
				castNode(AlterExtensionContentsStmt, stmt);

		utility_context->extension_lifecycle = strcmp(alter_stmt->extname,
													  "pg_textsearch") == 0;
	}
	else if (IsA(stmt, DropStmt))
	{
		DropStmt *drop_stmt = castNode(DropStmt, stmt);
		ListCell *lc;

		if (drop_stmt->removeType == OBJECT_EXTENSION)
		{
			foreach (lc, drop_stmt->objects)
			{
				List *name = lfirst(lc);

				if (list_length(name) == 1 &&
					strcmp(strVal(linitial(name)), "pg_textsearch") == 0)
				{
					utility_context->extension_lifecycle = true;
					break;
				}
			}
		}
	}

	if (utility_context->extension_lifecycle)
	{
		utility_context->serialize_rls_ddl	 = true;
		utility_context->retain_rls_ddl_lock = true;
	}
	else if (IsA(stmt, IndexStmt))
	{
		IndexStmt *index_stmt = castNode(IndexStmt, stmt);

		utility_context->track_index_build = true;
		utility_context->serialize_rls_ddl = index_stmt->accessMethod !=
													 NULL &&
											 strcmp(index_stmt->accessMethod,
													"bm25") == 0;
	}
	else if (IsA(stmt, ReindexStmt))
		utility_context->serialize_rls_ddl = true;
	else if (IsA(stmt, CreateStmt))
	{
		CreateStmt *create_stmt = castNode(CreateStmt, stmt);

		if (create_stmt->inhRelations != NIL)
		{
			utility_context->serialize_rls_ddl	   = true;
			utility_context->track_relation_create = true;
		}
	}
	else if (IsA(stmt, AlterTableStmt))
	{
		AlterTableStmt *alter_stmt = castNode(AlterTableStmt, stmt);
		ListCell	   *lc;

		foreach (lc, alter_stmt->cmds)
		{
			AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

			if (cmd->subtype == AT_EnableRowSecurity)
			{
				utility_context->check_rls_enable  = true;
				utility_context->serialize_rls_ddl = true;
			}
			else if (
					cmd->subtype == AT_AddInherit ||
					(cmd->subtype == AT_AttachPartition &&
					 alter_stmt->objtype == OBJECT_TABLE))
			{
				utility_context->check_hierarchy_change = true;
				utility_context->serialize_rls_ddl		= true;
			}
		}
	}
}

static bool
relation_exists(Oid relid)
{
	Relation	class_rel;
	ScanKeyData key;
	SysScanDesc scan;
	bool		exists;

	/* End triggers can delete the tuple in the current command. */
	class_rel = table_open(RelationRelationId, AccessShareLock);
	ScanKeyInit(
			&key,
			Anum_pg_class_oid,
			BTEqualStrategyNumber,
			F_OIDEQ,
			ObjectIdGetDatum(relid));
	scan = systable_beginscan(
			class_rel, ClassOidIndexId, true, SnapshotSelf, 1, &key);
	exists = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(class_rel, AccessShareLock);

	return exists;
}

static void
validate_utility_context(TpProcessUtilityContext *utility_context)
{
	ListCell *lc;

	foreach (lc, utility_context->altered_relids)
	{
		Oid relid = lfirst_oid(lc);

		if (relation_exists(relid))
			tp_check_rls_enable_allowed(relid);
	}

	foreach (lc, utility_context->hierarchy_relids)
	{
		Oid relid = lfirst_oid(lc);

		if (relation_exists(relid))
			tp_check_bm25_hierarchy_allowed(relid);
	}
}

static void
call_next_process_utility(
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

	if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *)parsetree;
		RoleSpec	   *new_owner;

		tp_reject_user_lineage_alter(stmt);
		new_owner = tp_alter_new_owner(stmt);
		if (new_owner != NULL &&
			(stmt->objtype == OBJECT_TABLE || stmt->objtype == OBJECT_MATVIEW))
		{
			Oid	  relation_oid;
			List *indexoids;

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

			relation_oid =
					RangeVarGetRelid(stmt->relation, NoLock, stmt->missing_ok);
			if (!OidIsValid(relation_oid))
				return;
			indexoids = tp_relation_indexes_locked(relation_oid, NoLock);
			tp_activate_background_indexes(indexoids, true);
			list_free(indexoids);
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

		if (stmt->kind != REINDEX_OBJECT_INDEX &&
			stmt->kind != REINDEX_OBJECT_TABLE)
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

		indexoids = tp_reindex_initial_indexes(stmt, &tracks_commits);

		PG_TRY();
		{
			if (tracks_commits)
				reindex_state = tp_reindex_tracking_begin(indexoids);
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
				tp_reconcile_reindex_states();
			else
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

	if (IsA(parsetree, IndexStmt))
	{
		IndexStmt *stmt = (IndexStmt *)parsetree;

		if (stmt->accessMethod && strcmp(stmt->accessMethod, "bm25") == 0)
		{
			Oid			heapoid;
			Relation	heap_rel;
			List	   *indexes_before;
			List	   *indexes_after;
			List	   *created_indexes;
			const char *compaction;

			compaction = tp_index_stmt_option(stmt, "compaction");
			heapoid	   = RangeVarGetRelidExtended(
					   stmt->relation,
					   stmt->concurrent ? ShareUpdateExclusiveLock : ShareLock,
					   0,
					   RangeVarCallbackOwnsRelation,
					   NULL);
			heap_rel = relation_open(heapoid, NoLock);
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

			indexes_before = tp_relation_tree_indexes_locked(
					heapoid,
					stmt->concurrent ? ShareUpdateExclusiveLock : ShareLock);
			relation_close(heap_rel, NoLock);

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

			/*
			 * CREATE INDEX CONCURRENTLY may cross transaction boundaries
			 * inside standard_ProcessUtility, so reacquire the heap lock
			 * before consulting its post-command relcache state.
			 */
			heapoid = RangeVarGetRelid(stmt->relation, AccessShareLock, false);
			indexes_after =
					tp_relation_tree_indexes_locked(heapoid, AccessShareLock);
			created_indexes =
					list_difference_oid(indexes_after, indexes_before);

			tp_activate_background_indexes(created_indexes, true);

			list_free(created_indexes);
			list_free(indexes_after);
			list_free(indexes_before);
			return;
		}
	}
	/* Not a bm25 CREATE INDEX - pass through */
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
 * ProcessUtility hook - isolate each utility command's object-access events,
 * post-validate ALTER TABLE catalog state, and preserve partitioned build
 * progress tracking.
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
	TpProcessUtilityContext *utility_context;

	utility_context =
			MemoryContextAllocZero(TopMemoryContext, sizeof(*utility_context));
	initialize_utility_context(utility_context, pstmt->utilityStmt);
	current_utility_context = utility_context;

	PG_TRY();
	{
		if (utility_context->serialize_rls_ddl)
		{
			utility_context->rls_ddl_lock_mode =
					utility_context->extension_lifecycle ||
									!utility_context->allow_rls
							? ExclusiveLock
							: ShareLock;
			utility_context->rls_ddl_lock_acquired = acquire_rls_ddl_lock(
					utility_context->rls_ddl_lock_mode,
					utility_context->previous != NULL,
					utility_context->extension_lifecycle,
					&utility_context->rls_ddl_lock_object);
		}

		call_next_process_utility(
				pstmt,
				queryString,
				readOnlyTree,
				context,
				params,
				queryEnv,
				dest,
				qc);

		validate_utility_context(utility_context);

		if (utility_context->build_progress_started)
		{
			utility_context->build_progress_started = false;
			tp_build_progress_end();
		}

		if (utility_context->rls_ddl_lock_acquired)
		{
			release_rls_ddl_lock(
					utility_context->rls_ddl_lock_object,
					utility_context->rls_ddl_lock_mode,
					utility_context->retain_rls_ddl_lock);
			utility_context->rls_ddl_lock_acquired = false;
		}

		current_utility_context = utility_context->previous;
		list_free(utility_context->altered_relids);
		list_free(utility_context->hierarchy_relids);
		pfree(utility_context);
	}
	PG_CATCH();
	{
		current_utility_context = utility_context->previous;
		if (utility_context->build_progress_started)
		{
			utility_context->build_progress_started = false;
			tp_build_progress_abort();
		}
		if (utility_context->rls_ddl_lock_acquired)
		{
			release_rls_ddl_lock(
					utility_context->rls_ddl_lock_object,
					utility_context->rls_ddl_lock_mode,
					false);
			utility_context->rls_ddl_lock_acquired = false;
		}
		pfree(utility_context);
		PG_RE_THROW();
	}
	PG_END_TRY();
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
