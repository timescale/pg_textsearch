#include <postgres.h>

#include <access/relation.h>
#include <access/reloptions.h>
#include <catalog/pg_class_d.h>
#include <miscadmin.h>
#include <storage/ipc.h>
#include <storage/shmem.h>
#include <utils/guc.h>
#include <utils/inval.h>

#include "constants.h"
#include "memtable.h"
#include "posting.h"
#include "registry.h"

PG_MODULE_MAGIC;

/* Relation options for Tapir indexes */
relopt_kind tp_relopt_kind;

/* External variable from limits module */
extern int tp_default_limit;

/* Global variable for score logging */
bool tp_log_scores = false;

/* Relcache invalidation callback for cleaning up shared memory */
static void tp_relcache_callback(Datum arg, Oid relid);

/* Previous shared memory startup hook */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Shared memory request hook */
static shmem_request_hook_type prev_shmem_request_hook = NULL;

/* Shared memory size calculation */
static void tp_shmem_request(void);

/* Shared memory startup hook */
static void tp_shmem_startup(void);

/*
 * Extension entry point - called when the extension is loaded
 */
void
_PG_init(void)
{
	/* Initialize on first use - don't require shared_preload_libraries */
	elog(DEBUG1,
		 "pg_textsearch extension _PG_init() starting GUC initialization");

	/*
	 * Define GUC parameters
	 */
	DefineCustomIntVariable(
			"pg_textsearch.index_memory_limit",
			"Per-index memory limit in MB (currently not enforced)",
			"Reserved for future use: controls the maximum memory each "
			"pg_textsearch index can use",
			&tp_index_memory_limit,
			TP_DEFAULT_INDEX_MEMORY_LIMIT, /* default 64MB */
			1,							   /* min 1MB */
			TP_MAX_INDEX_MEMORY_LIMIT,	   /* max 512MB */
			PGC_SIGHUP,					   /* Can be changed without restart */
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
			false,		 /* default off */
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

	elog(DEBUG1, "pg_textsearch extension GUC variables defined successfully");

	/*
	 * Install shared memory hooks (needed for registry)
	 */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook		= tp_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook		= tp_shmem_startup;

	elog(DEBUG1, "pg_textsearch shared memory hooks installed");

	/* Register relcache invalidation callback for index cleanup */
	CacheRegisterRelcacheCallback(tp_relcache_callback, (Datum)0);

	elog(DEBUG1, "pg_textsearch extension _PG_init() completed successfully");
}

/*
 * Relcache invalidation callback for cleaning up shared memory
 *
 * This is called whenever a relation's cache entry is invalidated, including
 * when indexes are dropped. We use this to clean up our DSA areas and shared
 * memory structures.
 *
 * Important notes:
 * - Relcache invalidations happen for many reasons (DDL, drops, etc.), not
 *   just drops. We must check if the relation still exists before cleaning
 *   up.
 * - We can do catalog lookups here if we're in a transaction context.
 * - This callback is invoked in EVERY backend process that receives the
 *   shared invalidation message, not just the backend that dropped the index.
 * - Our cleanup code must be idempotent and handle concurrent execution
 *   safely. The registry lookup with lock ensures only one backend actually
 *   performs the cleanup.
 */
static void
tp_relcache_callback(Datum arg, Oid relid)
{
	Relation rel;

	(void)arg; /* unused */

	/*
	 * Check if this relation is in our registry (meaning it's a tapir
	 * index). If not, we can ignore this invalidation.
	 */
	if (!tp_registry_is_registered(relid))
		return;

	/*
	 * Try to open the relation to see if it still exists. If it doesn't,
	 * this is a DROP and we should clean up. We use try_relation_open which
	 * returns NULL if the relation doesn't exist, rather than throwing an
	 * error.
	 */
	rel = try_relation_open(relid, NoLock);
	if (rel != NULL)
	{
		/* Relation still exists, just a normal invalidation */
		relation_close(rel, NoLock);
		return;
	}

	/* Relation doesn't exist - it was dropped, proceed with cleanup */

	/*
	 * Unregister from global registry first. This removes the entry
	 * atomically, so other backends seeing the same invalidation will not
	 * find it registered anymore.
	 */
	tp_registry_unregister(relid);

	/*
	 * Clean up index-specific shared memory (DSA areas, dshash tables, etc.)
	 * This is safe to call even if another backend already did the cleanup,
	 * as it handles missing entries gracefully.
	 */
	tp_cleanup_index_shared_memory(relid);
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

	/* Request shared memory for registry */
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

	/* Initialize the registry in shared memory */
	tp_registry_shmem_startup();
}
