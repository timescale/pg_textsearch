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
#include <catalog/pg_index.h>
#include <catalog/pg_inherits.h>
#include <catalog/pg_namespace_d.h>
#include <catalog/pg_tablespace_d.h>
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

typedef TpCompactionJobIdentity TpReindexTarget;

typedef struct TpReindexState
{
	MemoryContext		   context;
	List				  *targets;
	ReindexObjectType	   scope_kind;
	Oid					   scope_oid;
	bool				   reconciling;
	bool				   defer_reconciliation;
	bool				   post_publication;
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

static void tp_reindex_add_targets(TpReindexState *state, List *indexoids);

typedef TpCompactionJobIdentity TpOwnerChangeTarget;

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

/*
 * Managed lifecycle hooks collect transaction-local desired state here.
 * Reconciliation is delayed until a terminal barrier so every target can be
 * admitted before any pg_durable dependency or SPI work begins.
 */
static MemoryContext tp_managed_intent_context			= NULL;
static List			*tp_managed_intents					= NIL;
static bool			 tp_managed_reconciling				= false;
static bool			 tp_post_publication_reconciliation = false;

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
static void tp_collect_reindex_state_intents(bool include_deferred);
static bool tp_reconcile_managed_intents(void);
static void tp_reconcile_managed_intents_at_precommit(void);

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

static void
tp_copy_compaction_identity(
		TpCompactionJobIdentity		  *destination,
		const TpCompactionJobIdentity *source)
{
	memcpy(destination, source, sizeof(*destination));
	destination->index_name = source->index_name == NULL
									? NULL
									: pstrdup(source->index_name);
	destination->lineage	= source->lineage == NULL ? NULL
													  : pstrdup(source->lineage);
	destination->schedule	= source->schedule == NULL
									? NULL
									: pstrdup(source->schedule);
}

static void
tp_reset_compaction_identity(TpCompactionJobIdentity *identity)
{
	if (identity->index_name != NULL)
		pfree(identity->index_name);
	if (identity->lineage != NULL)
		pfree(identity->lineage);
	if (identity->schedule != NULL)
		pfree(identity->schedule);
	memset(identity, 0, sizeof(*identity));
}

static void
tp_replace_managed_string(char **destination, const char *source)
{
	if (*destination != NULL)
		pfree(*destination);
	*destination = source == NULL ? NULL : pstrdup(source);
}

static void
tp_update_managed_intent(
		TpManagedIndexIntent		  *intent,
		const TpCompactionJobIdentity *source,
		const char					  *schedule,
		const char					  *lineage,
		int							   flags)
{
	const int mode_flags = TP_MANAGED_INTENT_REFRESH_DEFAULT |
						   TP_MANAGED_INTENT_RECONCILE_OPTIONS |
						   TP_MANAGED_INTENT_PRESERVE_SCHEDULE |
						   TP_MANAGED_INTENT_DISABLE |
						   TP_MANAGED_INTENT_POST_PUBLICATION;
	bool current_options_authoritative =
			(intent->flags & (TP_MANAGED_INTENT_REFRESH_DEFAULT |
							  TP_MANAGED_INTENT_RECONCILE_OPTIONS)) != 0;
	bool pending_option_reconciliation =
			(intent->flags & TP_MANAGED_INTENT_RECONCILE_OPTIONS) != 0;
	int persistent_flags = (intent->flags | flags) &
						   TP_MANAGED_INTENT_LINEAGE_SUPPLIED;
	int new_mode_flags = flags & mode_flags;

	if ((flags & TP_MANAGED_INTENT_PRESERVE_SCHEDULE) != 0 &&
		pending_option_reconciliation)
		new_mode_flags |= TP_MANAGED_INTENT_RECONCILE_OPTIONS;
	intent->flags = persistent_flags | new_mode_flags;
	if ((flags & TP_MANAGED_INTENT_RECONCILE_OPTIONS) != 0)
	{
		tp_replace_managed_string(&intent->schedule, schedule);
		tp_replace_managed_string(&intent->lineage, lineage);
	}
	else if (
			(flags & TP_MANAGED_INTENT_LINEAGE_SUPPLIED) != 0 &&
			intent->lineage == NULL && lineage != NULL)
		intent->lineage = pstrdup(lineage);

	if ((flags & TP_MANAGED_INTENT_PRESERVE_SCHEDULE) != 0)
	{
		tp_reset_compaction_identity(&intent->source);
		if (source != NULL)
		{
			tp_copy_compaction_identity(&intent->source, source);
			if (pending_option_reconciliation)
				tp_replace_managed_string(
						&intent->source.schedule, intent->schedule);
			if (current_options_authoritative)
				intent->source.schedule_resolved = true;
		}
	}
}

static void
tp_collect_managed_intent_internal(
		Oid							   indexoid,
		const TpCompactionJobIdentity *source,
		const char					  *schedule,
		const char					  *lineage,
		int							   flags,
		bool						   validate_relation)
{
	MemoryContext		  old_context;
	SubTransactionId	  subid	 = GetCurrentSubTransactionId();
	TpManagedIndexIntent *intent = NULL;
	ListCell			 *lc;
	Relation			  index_rel;

	if (!OidIsValid(indexoid))
		return;

	if (validate_relation)
	{
		index_rel = try_relation_open(indexoid, AccessShareLock);
		if (index_rel == NULL)
			return;
		if (!RelationUsesLocalBuffers(index_rel))
		{
			relation_close(index_rel, AccessShareLock);
			index_rel = NULL;
		}
	}
	else
		index_rel = NULL;
	if (index_rel != NULL)
	{
		relation_close(index_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("background compaction is not supported for "
						"temporary indexes")));
	}

	if (tp_managed_intent_context == NULL)
		tp_managed_intent_context = AllocSetContextCreate(
				TopMemoryContext,
				"pg_textsearch managed intents",
				ALLOCSET_SMALL_SIZES);

	foreach (lc, tp_managed_intents)
	{
		TpManagedIndexIntent *candidate = lfirst(lc);

		if (candidate->index_oid == indexoid && candidate->subid == subid)
		{
			intent = candidate;
			break;
		}
	}

	old_context = MemoryContextSwitchTo(tp_managed_intent_context);
	if (intent == NULL)
	{
		intent			   = palloc0(sizeof(*intent));
		intent->index_oid  = indexoid;
		intent->subid	   = subid;
		tp_managed_intents = lappend(tp_managed_intents, intent);
	}
	tp_update_managed_intent(intent, source, schedule, lineage, flags);
	MemoryContextSwitchTo(old_context);
}

static void
tp_collect_managed_intent(
		Oid							   indexoid,
		const TpCompactionJobIdentity *source,
		const char					  *schedule,
		const char					  *lineage,
		int							   flags)
{
	tp_collect_managed_intent_internal(
			indexoid, source, schedule, lineage, flags, true);
}

static void
tp_collect_prevalidated_managed_intent(
		Oid							   indexoid,
		const TpCompactionJobIdentity *source,
		const char					  *schedule,
		const char					  *lineage,
		int							   flags)
{
	tp_collect_managed_intent_internal(
			indexoid, source, schedule, lineage, flags, false);
}

static void
tp_reset_managed_intents(void)
{
	if (tp_managed_intent_context != NULL)
		MemoryContextDelete(tp_managed_intent_context);
	tp_managed_intent_context		   = NULL;
	tp_managed_intents				   = NIL;
	tp_managed_reconciling			   = false;
	tp_post_publication_reconciliation = false;
}

static void
tp_free_managed_intent(TpManagedIndexIntent *intent)
{
	tp_reset_compaction_identity(&intent->source);
	if (intent->schedule != NULL)
		pfree(intent->schedule);
	if (intent->lineage != NULL)
		pfree(intent->lineage);
	pfree(intent);
}

static void
tp_abort_managed_intents(SubTransactionId subid)
{
	ListCell *lc;

	foreach (lc, tp_managed_intents)
	{
		TpManagedIndexIntent *intent = lfirst(lc);

		if (intent->subid == subid)
		{
			tp_managed_intents =
					foreach_delete_current(tp_managed_intents, lc);
			tp_free_managed_intent(intent);
		}
	}
}

static void
tp_merge_managed_intent(
		TpManagedIndexIntent *destination, const TpManagedIndexIntent *source)
{
	tp_update_managed_intent(
			destination,
			OidIsValid(source->source.index_oid) ? &source->source : NULL,
			source->schedule,
			source->lineage,
			source->flags);
}

static void
tp_promote_managed_intents(
		SubTransactionId subid, SubTransactionId parent_subid)
{
	MemoryContext old_context;
	List		 *merged = NIL;
	ListCell	 *lc;

	if (tp_managed_intents == NIL)
		return;

	old_context = MemoryContextSwitchTo(tp_managed_intent_context);
	foreach (lc, tp_managed_intents)
	{
		TpManagedIndexIntent *intent   = lfirst(lc);
		TpManagedIndexIntent *existing = NULL;
		ListCell			 *merged_lc;

		if (intent->subid == subid)
			intent->subid = parent_subid;
		foreach (merged_lc, merged)
		{
			TpManagedIndexIntent *candidate = lfirst(merged_lc);

			if (candidate->index_oid == intent->index_oid &&
				candidate->subid == intent->subid)
			{
				existing = candidate;
				break;
			}
		}
		if (existing != NULL)
		{
			tp_merge_managed_intent(existing, intent);
			tp_free_managed_intent(intent);
		}
		else
			merged = lappend(merged, intent);
	}
	list_free(tp_managed_intents);
	tp_managed_intents = merged;
	MemoryContextSwitchTo(old_context);
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
			tp_collect_reindex_state_intents(true);

		/*
		 * Check for bulk load auto-spill before commit.
		 * If any index had a large number of terms added this transaction,
		 * spill to disk to prevent unbounded memory growth.
		 */
		tp_bulk_load_spill_check();
		tp_reconcile_managed_intents_at_precommit();
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
		tp_reset_managed_intents();
		break;

	case XACT_EVENT_ABORT:
	case XACT_EVENT_PARALLEL_ABORT:
		/* Clean up any in-progress index builds (private DSA) */
		tp_cleanup_build_mode_on_abort();
		/* Release all index locks held by this backend */
		tp_release_all_index_locks();
		/* Reset bulk load counters for next transaction */
		tp_reset_bulk_load_counters();
		tp_reset_managed_intents();
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
		tp_abort_managed_intents(mySubid);
		break;

	case SUBXACT_EVENT_COMMIT_SUB:
		tp_promote_subxact_states(mySubid, parentSubid);
		tp_promote_managed_intents(mySubid, parentSubid);
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

	(void)heap_oid;
	(void)owner_oid;
	lineage		  = tp_new_compaction_lineage();
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
	if (mode == NULL || pg_strcasecmp(mode, "background") != 0)
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

		owner_oids = list_append_unique_oid(owner_oids, owner_oid);
	}
	return owner_oids;
}

static bool
tp_bulk_move_preflight(AlterTableMoveAllStmt *stmt, Oid *source_tablespace)
{
	AclResult aclresult;
	Oid		  destination_tablespace;

	*source_tablespace = get_tablespace_oid(stmt->orig_tablespacename, false);
	destination_tablespace =
			get_tablespace_oid(stmt->new_tablespacename, false);

	if (*source_tablespace == GLOBALTABLESPACE_OID ||
		destination_tablespace == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot move relations in to or out of pg_global "
						"tablespace")));

	if (OidIsValid(destination_tablespace) &&
		destination_tablespace != MyDatabaseTableSpace)
	{
		aclresult = object_aclcheck(
				TableSpaceRelationId,
				destination_tablespace,
				GetUserId(),
				ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(
					aclresult,
					OBJECT_TABLESPACE,
					get_tablespace_name(destination_tablespace));
	}

	if (*source_tablespace == MyDatabaseTableSpace)
		*source_tablespace = InvalidOid;
	if (destination_tablespace == MyDatabaseTableSpace)
		destination_tablespace = InvalidOid;

	return *source_tablespace != destination_tablespace;
}

static bool
tp_bulk_move_has_unauthorized_index(Oid tablespace_oid, List *owner_oids)
{
	Relation	class_rel;
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		unauthorized = false;

	class_rel = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(class_rel, InvalidOid, false, NULL, 0, NULL);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_class class_form = (Form_pg_class)GETSTRUCT(tuple);

		if (class_form->reltablespace != tablespace_oid ||
			(class_form->relkind != RELKIND_INDEX &&
			 class_form->relkind != RELKIND_PARTITIONED_INDEX) ||
			IsCatalogNamespace(class_form->relnamespace) ||
			class_form->relisshared ||
			isAnyTempNamespace(class_form->relnamespace) ||
			IsToastNamespace(class_form->relnamespace) ||
			(owner_oids != NIL &&
			 !list_member_oid(owner_oids, class_form->relowner)))
			continue;

		if (!object_ownercheck(
					RelationRelationId, class_form->oid, GetUserId()))
		{
			unauthorized = true;
			break;
		}
	}
	systable_endscan(scan);
	table_close(class_rel, AccessShareLock);
	return unauthorized;
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
tp_reindex_initial_indexes(
		ReindexStmt *stmt,
		bool		*tracks_commits,
		bool		 is_top_level,
		Oid			*scope_oid)
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
		*scope_oid		= namespace_oid;
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
		*scope_oid		= MyDatabaseId;
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
		{
			PreventInTransactionBlock(is_top_level, "REINDEX INDEX");
			return tp_index_tree_locked(relation_oid, ShareLock);
		}
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
	{
		PreventInTransactionBlock(is_top_level, "REINDEX TABLE");
		return tp_relation_tree_indexes_locked(relation_oid, ShareLock);
	}

	indexoids = tp_relation_indexes_locked(relation_oid, NoLock);
	return indexoids;
}

static void
tp_reindex_preflight(ReindexStmt *stmt, bool is_top_level)
{
	ListCell *lc;
	bool	  concurrently	  = false;
	char	 *tablespace_name = NULL;

	foreach (lc, stmt->params)
	{
		DefElem *option = lfirst_node(DefElem, lc);

		if (strcmp(option->defname, "verbose") == 0)
			(void)defGetBoolean(option);
		else if (strcmp(option->defname, "concurrently") == 0)
			concurrently = defGetBoolean(option);
		else if (strcmp(option->defname, "tablespace") == 0)
			tablespace_name = defGetString(option);
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized REINDEX option \"%s\"",
							option->defname)));
	}

	if (concurrently)
		PreventInTransactionBlock(is_top_level, "REINDEX CONCURRENTLY");

	if (tablespace_name != NULL)
	{
		AclResult aclresult;
		Oid		  tablespace_oid = get_tablespace_oid(tablespace_name, false);

		if (OidIsValid(tablespace_oid) &&
			tablespace_oid != MyDatabaseTableSpace)
		{
			aclresult = object_aclcheck(
					TableSpaceRelationId,
					tablespace_oid,
					GetUserId(),
					ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(
						aclresult,
						OBJECT_TABLESPACE,
						get_tablespace_name(tablespace_oid));
		}
	}
}

static void
tp_collect_background_indexes(List *indexoids, bool refresh_default)
{
	List	 *candidates;
	ListCell *lc;

	candidates = tp_physical_bm25_indexes(indexoids, true);
	foreach (lc, candidates)
		tp_collect_managed_intent(
				lfirst_oid(lc),
				NULL,
				NULL,
				NULL,
				refresh_default ? TP_MANAGED_INTENT_REFRESH_DEFAULT : 0);
	list_free(candidates);
}

static int
tp_lineage_cmp(const ListCell *left, const ListCell *right)
{
	return strcmp((const char *)lfirst(left), (const char *)lfirst(right));
}

static bool
tp_reconcile_managed_intents(void)
{
	List				   *frozen;
	List				   *indexoids = NIL;
	List				   *lineages  = NIL;
	List				   *locked;
	ListCell			   *lc;
	TpCompactionJobObjects *objects			 = NULL;
	bool					deferred		 = false;
	bool					post_publication = false;
	bool					snapshot_pushed	 = false;
	bool					strict_lineage	 = false;

	if (tp_managed_reconciling || tp_managed_intents == NIL)
		return true;

	frozen			   = tp_managed_intents;
	tp_managed_intents = NIL;
	foreach (lc, frozen)
	{
		TpManagedIndexIntent *intent = lfirst(lc);

		if ((intent->flags & TP_MANAGED_INTENT_DISABLE) == 0 ||
			(intent->flags & TP_MANAGED_INTENT_LINEAGE_SUPPLIED) != 0)
			indexoids = list_append_unique_oid(indexoids, intent->index_oid);
		if ((intent->flags & TP_MANAGED_INTENT_LINEAGE_SUPPLIED) != 0 &&
			SearchSysCacheExists1(RELOID, ObjectIdGetDatum(intent->index_oid)))
			strict_lineage = true;
		if ((intent->flags & TP_MANAGED_INTENT_POST_PUBLICATION) != 0)
			post_publication = true;
	}

	tp_managed_reconciling = true;
	PG_TRY();
	{
		if (!ActiveSnapshotSet())
		{
			PushActiveSnapshot(GetLatestSnapshot());
			snapshot_pushed = true;
		}
		locked = tp_try_prelock_compaction_indexes(indexoids);
		if (list_length(locked) < list_length(indexoids))
			deferred = true;
		if (strict_lineage && list_length(locked) < list_length(indexoids))
			ereport(ERROR,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
					 errmsg("could not validate background compaction "
							"lineage"),
					 errdetail("A required index is concurrently locked."),
					 errhint("Retry the command after the conflicting "
							 "transaction completes.")));
		foreach (lc, frozen)
		{
			TpManagedIndexIntent *intent = lfirst(lc);

			if ((intent->flags & TP_MANAGED_INTENT_LINEAGE_SUPPLIED) != 0 &&
				list_member_oid(locked, intent->index_oid))
				lineages = lappend(lineages, intent->lineage);
		}
		list_sort(lineages, tp_lineage_cmp);
		foreach (lc, lineages)
		{
			if (!tp_try_lock_compaction_lineage(lfirst(lc)))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not validate background compaction "
								"lineage"),
						 errdetail(
								 "A requested lineage is concurrently "
								 "locked."),
						 errhint("Retry the command after the conflicting "
								 "transaction completes.")));
		}
		foreach (lc, locked)
		{
			char *lineage =
					tp_ensure_index_compaction_lineage(lfirst_oid(lc), NULL);

			if (lineage != NULL)
				pfree(lineage);
		}
		foreach (lc, frozen)
		{
			TpManagedIndexIntent *intent = lfirst(lc);

			if (!list_member_oid(locked, intent->index_oid) ||
				(intent->flags & TP_MANAGED_INTENT_DISABLE) != 0 ||
				(intent->flags & TP_MANAGED_INTENT_RECONCILE_OPTIONS) == 0 ||
				(intent->flags & TP_MANAGED_INTENT_LINEAGE_SUPPLIED) != 0)
				continue;
			tp_reconcile_index_compaction_options(
					intent->index_oid, intent->schedule, intent->lineage);
		}
		if (locked != NIL && (objects = tp_compaction_job_try_lock_objects(
									  !post_publication)) == NULL)
		{
			if (strict_lineage)
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not validate background compaction "
								"lineage"),
						 errdetail(
								 "A required extension object is concurrently "
								 "locked."),
						 errhint("Retry the command after the conflicting "
								 "transaction completes.")));
			list_free(locked);
			locked	 = NIL;
			deferred = true;
		}
		foreach (lc, frozen)
		{
			TpManagedIndexIntent *intent = lfirst(lc);
			Relation			  index_rel;
			Oid					  heap_oid;
			Oid					  owner_oid;

			if ((intent->flags & TP_MANAGED_INTENT_LINEAGE_SUPPLIED) == 0 ||
				!list_member_oid(locked, intent->index_oid))
				continue;
			index_rel = try_relation_open(intent->index_oid, NoLock);
			if (index_rel == NULL || index_rel->rd_index == NULL)
			{
				if (index_rel != NULL)
					relation_close(index_rel, NoLock);
				continue;
			}
			heap_oid  = index_rel->rd_index->indrelid;
			owner_oid = index_rel->rd_rel->relowner;
			relation_close(index_rel, NoLock);
			if (tp_compaction_lineage_in_use_by_other(
						intent->lineage, intent->index_oid, heap_oid) ||
				tp_compaction_job_lineage_exists(
						objects, intent->lineage, heap_oid, owner_oid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("background compaction lineage is already "
								"in use")));
		}
		foreach (lc, frozen)
		{
			TpManagedIndexIntent *intent = lfirst(lc);

			if (!list_member_oid(locked, intent->index_oid) ||
				(intent->flags & TP_MANAGED_INTENT_DISABLE) != 0 ||
				!tp_is_background_physical_index(intent->index_oid))
				continue;
			if ((intent->flags & (TP_MANAGED_INTENT_RECONCILE_OPTIONS |
								  TP_MANAGED_INTENT_LINEAGE_SUPPLIED)) ==
				(TP_MANAGED_INTENT_RECONCILE_OPTIONS |
				 TP_MANAGED_INTENT_LINEAGE_SUPPLIED))
				tp_reconcile_index_compaction_options(
						intent->index_oid, intent->schedule, intent->lineage);
			if ((intent->flags & TP_MANAGED_INTENT_PRESERVE_SCHEDULE) != 0)
			{
				tp_compaction_job_resolve_schedule(
						objects,
						intent->index_oid,
						&intent->source,
						tp_managed_intent_context);
				tp_compaction_job_activate_with_schedule(
						objects, intent->index_oid, intent->source.schedule);
			}
			else
				tp_compaction_job_activate(
						objects,
						intent->index_oid,
						(intent->flags & TP_MANAGED_INTENT_REFRESH_DEFAULT) !=
								0);
		}
		list_free(locked);
		if (tp_managed_intents != NIL)
			ereport(ERROR,
					(errmsg("cannot execute DDL during background compaction "
							"reconciliation")));
		if (deferred)
			ereport(WARNING,
					(errmsg("background compaction lifecycle reconciliation "
							"was deferred"),
					 errdetail(
							 "A required relation or extension object was "
							 "concurrently locked."),
					 errhint("Repeat the managed DDL after the conflicting "
							 "transaction completes.")));
		if (snapshot_pushed)
		{
			PopActiveSnapshot();
			snapshot_pushed = false;
		}
	}
	PG_FINALLY();
	{
		if (snapshot_pushed)
			PopActiveSnapshot();
		tp_managed_reconciling = false;
		list_free(indexoids);
		list_free(lineages);
		list_free(frozen);
	}
	PG_END_TRY();

	return true;
}

static void
tp_reconcile_managed_intents_at_precommit(void)
{
	MemoryContext old_context = CurrentMemoryContext;
	ResourceOwner old_owner	  = CurrentResourceOwner;
	ListCell	 *lc;
	bool		  post_publication = tp_post_publication_reconciliation;

	foreach (lc, tp_managed_intents)
	{
		TpManagedIndexIntent *intent = lfirst(lc);

		if ((intent->flags & TP_MANAGED_INTENT_POST_PUBLICATION) != 0)
		{
			post_publication = true;
			break;
		}
	}
	if (!post_publication)
	{
		tp_reconcile_managed_intents();
		return;
	}

	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		MemoryContextSwitchTo(old_context);
		tp_reconcile_managed_intents();
		tp_post_publication_reconciliation = false;
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old_context);
		CurrentResourceOwner = old_owner;
	}
	PG_CATCH();
	{
		ErrorData *edata;

		MemoryContextSwitchTo(old_context);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(old_context);
		CurrentResourceOwner = old_owner;
		tp_reset_managed_intents();
		if (edata->elevel >= FATAL ||
			edata->sqlerrcode == ERRCODE_QUERY_CANCELED ||
			edata->sqlerrcode == ERRCODE_ADMIN_SHUTDOWN ||
			edata->sqlerrcode == ERRCODE_CRASH_SHUTDOWN ||
			edata->sqlerrcode == ERRCODE_CANNOT_CONNECT_NOW)
			ReThrowError(edata);
		ereport(WARNING,
				(errmsg("background compaction lifecycle reconciliation was "
						"deferred"),
				 errdetail(
						 "Managed reconciliation failed after core "
						 "publication: %s.",
						 edata->message),
				 errhint("Repeat the managed DDL after correcting the "
						 "reported condition.")));
		FreeErrorData(edata);
	}
	PG_END_TRY();
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

		target = palloc0(sizeof(*target));
		tp_compaction_job_capture(indexoid, target);
		targets = lappend(targets, target);
	}
	return targets;
}

static void
tp_collect_owner_change_targets(List *targets)
{
	ListCell *lc;

	foreach (lc, targets)
	{
		TpOwnerChangeTarget *target = lfirst(lc);

		if (tp_is_background_physical_index(target->index_oid))
			tp_collect_managed_intent(
					target->index_oid,
					target,
					NULL,
					NULL,
					TP_MANAGED_INTENT_PRESERVE_SCHEDULE);
	}
}

static void
tp_free_owner_change_targets(List *targets)
{
	ListCell *lc;

	foreach (lc, targets)
	{
		TpOwnerChangeTarget *target = lfirst(lc);

		pfree(target->index_name);
		pfree(target->lineage);
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
tp_collect_created_background_indexes(
		List	   *created_indexes,
		Oid			heap_oid,
		const char *schedule,
		const char *lineage,
		bool		lineage_supplied)
{
	List	 *index_tree;
	List	 *physical_indexes;
	ListCell *lc;

	if (lineage == NULL)
		elog(ERROR, "background index has no compaction lineage");

	index_tree		 = tp_created_index_tree_locked(created_indexes, heap_oid);
	physical_indexes = tp_physical_bm25_indexes(index_tree, false);
	list_free(index_tree);

	foreach (lc, physical_indexes)
		tp_collect_managed_intent(
				lfirst_oid(lc),
				NULL,
				schedule,
				lineage,
				TP_MANAGED_INTENT_REFRESH_DEFAULT |
						TP_MANAGED_INTENT_RECONCILE_OPTIONS |
						(lineage_supplied ? TP_MANAGED_INTENT_LINEAGE_SUPPLIED
										  : 0));
	list_free(physical_indexes);
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
		tp_collect_created_background_indexes(
				index_tree, relation_oid, schedule, lineage, false);
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

	tp_collect_created_background_indexes(
			index_tree, heap_oid, schedule, lineage, false);
	list_free(index_tree);
	if (schedule != NULL)
		pfree(schedule);
	pfree(lineage);
}

static TpReindexState *
tp_reindex_tracking_begin(
		List			 *indexoids,
		ReindexObjectType scope_kind,
		Oid				  scope_oid,
		bool			  defer_reconciliation,
		bool			  post_publication)
{
	MemoryContext	caller_context = CurrentMemoryContext;
	MemoryContext	context;
	TpReindexState *state = NULL;

	context = AllocSetContextCreate(
			TopMemoryContext, "pg_textsearch reindex", ALLOCSET_SMALL_SIZES);
	PG_TRY();
	{
		state			  = MemoryContextAllocZero(context, sizeof(*state));
		state->context	  = context;
		state->previous	  = tp_reindex_states;
		state->scope_kind = scope_kind;
		state->scope_oid  = scope_oid;
		state->defer_reconciliation = defer_reconciliation;
		state->post_publication		= post_publication;

		tp_reindex_add_targets(state, indexoids);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(caller_context);
		MemoryContextDelete(context);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (state->targets == NIL && !OidIsValid(state->scope_oid))
	{
		MemoryContextDelete(context);
		return NULL;
	}

	tp_reindex_states = state;
	return state;
}

static bool
tp_reindex_target_equals(
		const TpReindexTarget *left, const TpReindexTarget *right)
{
	if (left->index_oid == right->index_oid)
		return true;
	return left->heap_oid == right->heap_oid && left->lineage != NULL &&
		   right->lineage != NULL &&
		   strcmp(left->lineage, right->lineage) == 0;
}

static void
tp_reindex_add_targets(TpReindexState *state, List *indexoids)
{
	ListCell *lc;

	foreach (lc, indexoids)
	{
		Oid				 indexoid = lfirst_oid(lc);
		TpReindexTarget *target;
		MemoryContext	 old_context;
		ListCell		*target_cell;
		bool			 duplicate = false;

		foreach (target_cell, state->targets)
		{
			TpReindexTarget *existing = lfirst(target_cell);

			if (existing->index_oid == indexoid)
			{
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;

		old_context = MemoryContextSwitchTo(state->context);
		target		= palloc0(sizeof(*target));
		tp_compaction_job_capture(indexoid, target);
		foreach (target_cell, state->targets)
		{
			if (tp_reindex_target_equals(lfirst(target_cell), target))
			{
				duplicate = true;
				break;
			}
		}
		if (duplicate)
		{
			tp_reset_compaction_identity(target);
			pfree(target);
		}
		else
			state->targets = lappend(state->targets, target);
		MemoryContextSwitchTo(old_context);
	}
}

static void
tp_reindex_refresh_scope_targets(TpReindexState *state)
{
	List *indexoids;
	List *candidates;

	if (!OidIsValid(state->scope_oid))
		return;

	if (state->scope_kind == REINDEX_OBJECT_SCHEMA)
		indexoids = tp_namespace_bm25_indexes(state->scope_oid);
	else
	{
		Assert(state->scope_kind == REINDEX_OBJECT_DATABASE);
		indexoids = tp_all_bm25_indexes(InvalidOid, false, NIL, false);
	}
	candidates = tp_physical_bm25_indexes(indexoids, true);
	tp_reindex_add_targets(state, candidates);
	list_free(candidates);
	list_free(indexoids);
}

static Relation
tp_reindex_try_relation_open(Oid relation_oid, bool *contended)
{
	Relation relation;

	if (!ConditionalLockRelationOid(relation_oid, AccessShareLock))
	{
		*contended = true;
		return NULL;
	}
	relation = try_relation_open(relation_oid, NoLock);
	if (relation == NULL)
		UnlockRelationOid(relation_oid, AccessShareLock);
	return relation;
}

static void
tp_reindex_relation_close(Relation relation)
{
	Oid relation_oid = RelationGetRelid(relation);

	relation_close(relation, NoLock);
	UnlockRelationOid(relation_oid, AccessShareLock);
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
		   (target->lineage_backfilled ||
			(lineage != NULL && strcmp(lineage, target->lineage) == 0));
}

static bool
tp_reindex_target_live_original(
		TpReindexState	*state,
		TpReindexTarget *target,
		Oid				*indexoid,
		Oid				*tablespace_oid,
		RelFileNumber	*relfilenumber,
		bool			*contended)
{
	Relation index_rel;

	*indexoid = InvalidOid;
	index_rel = tp_reindex_try_relation_open(target->index_oid, contended);
	if (index_rel == NULL)
		return false;

	if (index_rel->rd_rel->relkind != RELKIND_INDEX ||
		!tp_reindex_target_matches(target, index_rel))
	{
		tp_reindex_relation_close(index_rel);
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
		tp_reindex_relation_close(index_rel);
		return false;
	}

	if (tp_is_background_physical_index_relation(index_rel))
	{
		tp_reindex_target_refresh_identity(state, target, index_rel);
		*indexoid		= target->index_oid;
		*tablespace_oid = index_rel->rd_locator.spcOid;
		*relfilenumber	= index_rel->rd_locator.relNumber;
	}
	tp_reindex_relation_close(index_rel);
	return true;
}

static bool
tp_reindex_collect_candidates(
		TpReindexState *state, List **candidates, List **indexoids)
{
	ListCell *lc;
	bool	  contended = false;

	foreach (lc, state->targets)
	{
		TpReindexTarget	   *target = lfirst(lc);
		TpReindexCandidate *candidate;
		Relation			index_rel;
		Oid					indexoid;
		Oid					tablespace_oid;
		RelFileNumber		relfilenumber;

		if (!tp_reindex_target_live_original(
					state,
					target,
					&indexoid,
					&tablespace_oid,
					&relfilenumber,
					&contended))
		{
			indexoid = get_relname_relid(
					target->index_name, target->namespace_oid);
			if (!OidIsValid(indexoid))
				continue;

			index_rel = tp_reindex_try_relation_open(indexoid, &contended);
			if (index_rel == NULL)
				continue;
			if (!tp_is_background_physical_index_relation(index_rel) ||
				!tp_reindex_target_matches(target, index_rel))
			{
				tp_reindex_relation_close(index_rel);
				continue;
			}

			tp_reindex_target_refresh_identity(state, target, index_rel);
			tp_reindex_relation_close(index_rel);
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
	return contended;
}

static void
tp_collect_reindex_state_intents(bool include_deferred)
{
	List		   *candidates = NIL;
	List		   *indexoids  = NIL;
	ListCell	   *lc;
	TpReindexState *state;
	bool			deferred = false;

	for (state = tp_reindex_states; state != NULL; state = state->previous)
	{
		if (state->reconciling ||
			(state->defer_reconciliation && !include_deferred))
			continue;
		tp_reindex_refresh_scope_targets(state);
		if (state->post_publication)
			tp_post_publication_reconciliation = true;
		if (tp_reindex_collect_candidates(state, &candidates, &indexoids))
		{
			if (!state->post_publication)
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not inspect a managed index after "
								"physical rewrite"),
						 errhint("Retry the command after the conflicting "
								 "transaction completes.")));
			deferred = true;
		}
	}

	foreach (lc, candidates)
	{
		TpReindexCandidate *candidate = lfirst(lc);

		tp_collect_prevalidated_managed_intent(
				candidate->index_oid,
				candidate->target,
				NULL,
				NULL,
				TP_MANAGED_INTENT_PRESERVE_SCHEDULE |
						TP_MANAGED_INTENT_POST_PUBLICATION);
	}
	list_free(indexoids);
	list_free_deep(candidates);
	if (deferred)
		ereport(WARNING,
				(errmsg("background compaction lifecycle reconciliation was "
						"deferred"),
				 errdetail(
						 "A published managed index could not be inspected "
						 "without waiting."),
				 errhint("Repeat the managed DDL after the conflicting "
						 "transaction completes.")));
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
tp_vacuum_option_enabled(VacuumStmt *stmt, const char *name)
{
	ListCell *lc;

	foreach (lc, stmt->options)
	{
		DefElem *option = lfirst_node(DefElem, lc);

		if (strcmp(option->defname, name) == 0)
			return defGetBoolean(option);
	}
	return false;
}

static bool
tp_vacuum_rewrites_storage(VacuumStmt *stmt)
{
	return stmt->is_vacuumcmd && tp_vacuum_option_enabled(stmt, "full");
}

static bool
tp_vacuum_can_maintain_relation(Oid relation_oid)
{
	return object_ownercheck(DatabaseRelationId, MyDatabaseId, GetUserId()) ||
		   tp_can_maintain_relation(relation_oid);
}

static List *
tp_vacuum_skip_locked_relation_indexes(Oid relation_oid, bool include_children)
{
	List	 *relation_oids;
	List	 *indexoids = NIL;
	ListCell *lc;

	if (include_children)
		relation_oids = find_all_inheritors(relation_oid, NoLock, NULL);
	else
		relation_oids = list_make1_oid(relation_oid);

	foreach (lc, relation_oids)
	{
		Oid		 child_oid = lfirst_oid(lc);
		Relation child;
		List	*child_indexes;

		if (child_oid != relation_oid &&
			!ConditionalLockRelationOid(child_oid, AccessShareLock))
			continue;
		if (!tp_vacuum_can_maintain_relation(child_oid))
			continue;

		child = try_relation_open(child_oid, NoLock);
		if (child == NULL)
			continue;
		child_indexes = list_copy(RelationGetIndexList(child));
		relation_close(child, NoLock);
		indexoids = list_concat_unique_oid(indexoids, child_indexes);
	}
	list_free(relation_oids);
	return indexoids;
}

static List *
tp_vacuum_relation_indexes_locked(Oid relation_oid, bool include_children)
{
	List	 *relation_oids;
	List	 *indexoids = NIL;
	ListCell *lc;

	if (include_children)
		relation_oids =
				find_all_inheritors(relation_oid, AccessExclusiveLock, NULL);
	else
		relation_oids = list_make1_oid(relation_oid);

	foreach (lc, relation_oids)
	{
		Oid		 child_oid = lfirst_oid(lc);
		Relation child;
		List	*child_indexes;

		if (!tp_vacuum_can_maintain_relation(child_oid))
			continue;

		child = try_relation_open(child_oid, NoLock);
		if (child == NULL)
			continue;
		child_indexes = list_copy(RelationGetIndexList(child));
		relation_close(child, NoLock);
		indexoids = list_concat_unique_oid(indexoids, child_indexes);
	}
	list_free(relation_oids);
	return indexoids;
}

static List *
tp_vacuum_skip_locked_all_indexes(void)
{
	List	 *all_indexes;
	List	 *indexoids		= NIL;
	List	 *locked_heaps	= NIL;
	List	 *skipped_heaps = NIL;
	ListCell *lc;

	all_indexes = tp_all_bm25_indexes(InvalidOid, false, NIL, false);
	foreach (lc, all_indexes)
	{
		Oid index_oid = lfirst_oid(lc);
		Oid heap_oid  = IndexGetRelation(index_oid, true);

		if (!OidIsValid(heap_oid) || list_member_oid(skipped_heaps, heap_oid))
			continue;
		if (!list_member_oid(locked_heaps, heap_oid))
		{
			if (!ConditionalLockRelationOid(heap_oid, AccessShareLock))
			{
				skipped_heaps = lappend_oid(skipped_heaps, heap_oid);
				continue;
			}
			locked_heaps = lappend_oid(locked_heaps, heap_oid);
		}
		if (tp_vacuum_can_maintain_relation(heap_oid))
			indexoids = lappend_oid(indexoids, index_oid);
	}
	list_free(skipped_heaps);
	list_free(locked_heaps);
	list_free(all_indexes);
	return indexoids;
}

static List *
tp_vacuum_rewrite_indexes(VacuumStmt *stmt)
{
	List	 *indexoids = NIL;
	ListCell *lc;
	bool	  skip_locked = tp_vacuum_option_enabled(stmt, "skip_locked");

	if (stmt->rels == NIL)
	{
		if (skip_locked)
			return tp_vacuum_skip_locked_all_indexes();
		return tp_maintainable_indexes(
				tp_all_bm25_indexes(InvalidOid, false, NIL, false), true);
	}

	foreach (lc, stmt->rels)
	{
		VacuumRelation *vacuum_rel	 = lfirst_node(VacuumRelation, lc);
		Oid				relation_oid = vacuum_rel->oid;
		List		   *relation_indexes;

		if (!OidIsValid(relation_oid) && vacuum_rel->relation != NULL)
			relation_oid = RangeVarGetRelidExtended(
					vacuum_rel->relation,
					AccessShareLock,
					RVR_MISSING_OK | (skip_locked ? RVR_SKIP_LOCKED : 0),
					NULL,
					NULL);
		else if (
				skip_locked && OidIsValid(relation_oid) &&
				!ConditionalLockRelationOid(relation_oid, AccessShareLock))
			continue;
		if (!OidIsValid(relation_oid))
			continue;

		if (skip_locked)
			relation_indexes = tp_vacuum_skip_locked_relation_indexes(
					relation_oid,
					vacuum_rel->relation != NULL && vacuum_rel->relation->inh);
		else
			relation_indexes = tp_vacuum_relation_indexes_locked(
					relation_oid,
					vacuum_rel->relation != NULL && vacuum_rel->relation->inh);
		indexoids = list_concat_unique_oid(indexoids, relation_indexes);
	}
	return indexoids;
}

static List *
tp_global_cluster_bm25_indexes(void)
{
	Relation	index_rel;
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *bm25_indexes;
	List	   *clustered_heaps = NIL;
	List	   *indexoids		= NIL;
	ListCell   *lc;

	index_rel = table_open(IndexRelationId, AccessShareLock);
	scan = systable_beginscan(index_rel, InvalidOid, false, NULL, 0, NULL);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_index index_form = (Form_pg_index)GETSTRUCT(tuple);

		if (!index_form->indisclustered ||
			!tp_can_maintain_relation(index_form->indrelid))
			continue;

		clustered_heaps =
				list_append_unique_oid(clustered_heaps, index_form->indrelid);
	}
	systable_endscan(scan);
	table_close(index_rel, AccessShareLock);

	bm25_indexes = tp_all_bm25_indexes(InvalidOid, false, NIL, false);
	foreach (lc, bm25_indexes)
	{
		Oid indexoid = lfirst_oid(lc);
		Oid heap_oid = IndexGetRelation(indexoid, true);

		if (OidIsValid(heap_oid) && list_member_oid(clustered_heaps, heap_oid))
			indexoids = lappend_oid(indexoids, indexoid);
	}
	list_free(bm25_indexes);
	list_free(clustered_heaps);
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
		return tp_global_cluster_bm25_indexes();
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
tp_alter_table_rewrite_recurses(AlterTableStmt *stmt)
{
	ListCell *lc;

	if (!stmt->relation->inh)
		return false;

	foreach (lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		switch (cmd->subtype)
		{
		case AT_AddColumn:
		case AT_AlterColumnType:
		case AT_SetExpression:
			return true;
		default:
			break;
		}
	}
	return false;
}

static List *
tp_alter_table_rewrite_indexes(
		AlterTableStmt *stmt, Oid relation_oid, LOCKMODE lockmode)
{
	List	 *relation_oids;
	List	 *indexoids = NIL;
	ListCell *lc;

	if (!tp_alter_table_rewrite_recurses(stmt))
		return tp_relation_indexes_locked(relation_oid, NoLock);

	relation_oids = find_all_inheritors(relation_oid, lockmode, NULL);
	foreach (lc, relation_oids)
	{
		Oid	  child_oid = lfirst_oid(lc);
		List *child_indexes;

		child_indexes = tp_relation_indexes_locked(child_oid, NoLock);
		indexoids	  = list_concat_unique_oid(indexoids, child_indexes);
	}
	list_free(relation_oids);
	return indexoids;
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

	(void)nowait;
	candidates	  = tp_physical_bm25_indexes(indexoids, true);
	rewrite_state = tp_reindex_tracking_begin(
			candidates, REINDEX_OBJECT_INDEX, InvalidOid, false, false);
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

		tp_collect_reindex_state_intents(true);
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

static void
tp_alter_owner_preflight(Oid relation_oid, RoleSpec *new_owner)
{
	HeapTuple	  relation_tuple;
	Form_pg_class relation_form;
	Oid			  new_owner_oid = get_rolespec_oid(new_owner, false);

	relation_tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relation_oid));
	if (!HeapTupleIsValid(relation_tuple))
		elog(ERROR, "cache lookup failed for relation %u", relation_oid);
	relation_form = (Form_pg_class)GETSTRUCT(relation_tuple);

	if (relation_form->relowner != new_owner_oid && !superuser())
	{
		AclResult aclresult;

		check_can_set_role(GetUserId(), new_owner_oid);
		aclresult = object_aclcheck(
				NamespaceRelationId,
				relation_form->relnamespace,
				new_owner_oid,
				ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(
					aclresult,
					OBJECT_SCHEMA,
					get_namespace_name(relation_form->relnamespace));
	}
	ReleaseSysCache(relation_tuple);
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
	if (tp_managed_reconciling && !IsA(pstmt->utilityStmt, GrantStmt))
		ereport(ERROR,
				(errmsg("cannot execute DDL during background compaction "
						"reconciliation")));

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
			Oid	  tablespace_oid;
			List *owner_oids = tp_role_oids(stmt->roles);

			if (tp_bulk_move_preflight(stmt, &tablespace_oid))
			{
				tablespace_oid = tp_catalog_tablespace_oid(tablespace_oid);
				if (superuser() || !tp_bulk_move_has_unauthorized_index(
										   tablespace_oid, owner_oids))
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
							tp_all_bm25_indexes(
									tablespace_oid,
									true,
									owner_oids,
									!superuser()),
							stmt->nowait);
					list_free(owner_oids);
					return;
				}
			}
			list_free(owner_oids);
		}
	}

	if (IsA(parsetree, ReassignOwnedStmt))
	{
		List *indexoids = tp_reassign_owned_indexes(
				castNode(ReassignOwnedStmt, parsetree));
		List *candidates = tp_physical_bm25_indexes(indexoids, true);
		List *owner_targets;

		tp_prelock_owner_change_heaps(candidates);
		owner_targets = tp_capture_owner_change_targets(candidates);
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

			tp_collect_owner_change_targets(owner_targets);
		}
		PG_FINALLY();
		{
			tp_free_owner_change_targets(owner_targets);
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
						tp_alter_table_rewrite_indexes(
								stmt, relation_oid, lockmode),
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

			relation_oid = AlterTableLookupRelation(
					stmt, AlterTableGetLockLevel(stmt->cmds));
			if (OidIsValid(relation_oid) &&
				object_ownercheck(
						RelationRelationId, relation_oid, GetUserId()))
			{
				List *indexoids;
				List *candidates;

				tp_alter_owner_preflight(relation_oid, new_owner);
				indexoids  = tp_relation_indexes_locked(relation_oid, NoLock);
				candidates = tp_physical_bm25_indexes(indexoids, true);
				owner_targets = tp_capture_owner_change_targets(candidates);
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

				tp_collect_owner_change_targets(owner_targets);
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
				tp_collect_managed_intent(
						indexoid,
						NULL,
						NULL,
						NULL,
						TP_MANAGED_INTENT_REFRESH_DEFAULT);
			else
				tp_collect_managed_intent(
						indexoid, NULL, NULL, NULL, TP_MANAGED_INTENT_DISABLE);
			return;
		}
	}

	if (IsA(parsetree, ReindexStmt))
	{
		ReindexStmt	   *stmt = (ReindexStmt *)parsetree;
		List		   *indexoids;
		bool			tracks_commits;
		TpReindexState *reindex_state = NULL;
		bool			is_top_level  = context == PROCESS_UTILITY_TOPLEVEL;
		Oid				scope_oid	  = InvalidOid;

		tp_reindex_preflight(stmt, is_top_level);
		if (stmt->kind == REINDEX_OBJECT_SCHEMA ||
			stmt->kind == REINDEX_OBJECT_DATABASE)
			PreventInTransactionBlock(is_top_level, "REINDEX");

		indexoids = tp_reindex_initial_indexes(
				stmt, &tracks_commits, is_top_level, &scope_oid);

		PG_TRY();
		{
			if (tracks_commits)
			{
				List *candidates = tp_physical_bm25_indexes(indexoids, true);

				reindex_state = tp_reindex_tracking_begin(
						candidates,
						stmt->kind,
						scope_oid,
						OidIsValid(scope_oid),
						true);
				list_free(candidates);
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
				tp_collect_reindex_state_intents(true);
			else if (
					stmt->kind == REINDEX_OBJECT_INDEX ||
					stmt->kind == REINDEX_OBJECT_TABLE)
			{
				indexoids = tp_reindex_current_indexes(stmt);
				tp_collect_background_indexes(indexoids, true);
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

			tp_collect_background_indexes(create_state->created_indexes, true);
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
			bool				lineage_supplied;

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
			lineage_supplied =
					tp_index_stmt_option(stmt, "compaction_lineage") != NULL;
			if (compaction != NULL &&
				pg_strcasecmp(compaction, "background") == 0 &&
				RelationUsesLocalBuffers(heap_rel))
			{
				relation_close(heap_rel, NoLock);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("background compaction is not supported for "
								"temporary indexes")));
			}
			if (stmt->concurrent && lineage_supplied && compaction != NULL &&
				pg_strcasecmp(compaction, "background") == 0)
			{
				relation_close(heap_rel, NoLock);
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("explicit background compaction lineage is "
								"not supported with CREATE INDEX "
								"CONCURRENTLY"),
						 errhint("Create the index without CONCURRENTLY, or "
								 "omit compaction_lineage.")));
			}
			if (compaction != NULL &&
				pg_strcasecmp(compaction, "background") == 0)
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
					pg_strcasecmp(compaction, "background") == 0)
					tp_collect_created_background_indexes(
							create_state->created_indexes,
							create_state->heap_oid,
							tp_index_stmt_option(stmt, "compaction_schedule"),
							tp_index_stmt_option(stmt, "compaction_lineage"),
							lineage_supplied);
				else
					tp_collect_background_indexes(
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
