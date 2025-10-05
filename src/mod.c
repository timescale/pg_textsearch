#include <postgres.h>

#include <access/relation.h>
#include <access/reloptions.h>
#include <catalog/dependency.h>
#include <catalog/objectaccess.h>
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
#include "state.h"

PG_MODULE_MAGIC;

/* Relation options for Tapir indexes */
relopt_kind tp_relopt_kind;

/* External variable from limits module */
extern int tp_default_limit;

/* Global variable for score logging */
bool tp_log_scores = false;

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

/* Relcache invalidation callback for cleaning up local state */
static void tp_relcache_callback(Datum arg, Oid relid);

/* Process exit callback for DSA cleanup */
/* static void tp_proc_exit(int code, Datum arg); -- unused, see comment below
 */

/* Flag to track if we've already registered callbacks */
static bool callbacks_registered = false;

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

	/* Install object access hook for DROP INDEX detection */
	prev_object_access_hook = object_access_hook;
	object_access_hook		= tp_object_access;

	/* Register callbacks only once per backend */
	if (!callbacks_registered)
	{
		/* Register relcache invalidation callback for local state cleanup */
		CacheRegisterRelcacheCallback(tp_relcache_callback, (Datum)0);

		/*
		 * Note: We don't register a process exit callback to detach from DSA
		 * because:
		 * 1. The OS cleans up all process resources on exit anyway
		 * 2. DROP EXTENSION might have already destroyed the DSA
		 * 3. Trying to detach from a destroyed DSA causes crashes
		 * 4. The DSA system has its own cleanup mechanisms
		 */
		/* on_proc_exit(tp_proc_exit, 0); -- disabled to prevent crashes */

		callbacks_registered = true;
		elog(DEBUG1, "pg_textsearch callbacks registered");
	}

	elog(DEBUG1, "pg_textsearch extension _PG_init() completed successfully");
}

/*
 * Note: We intentionally do not implement _PG_fini because:
 * 1. It's called when any backend exits (not just on DROP EXTENSION)
 * 2. DSA cleanup happens automatically via process exit callbacks
 * 3. Local memory is freed automatically when the backend exits
 * 4. Shared memory structures remain valid for other backends
 */

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
	/* Call previous hook if exists */
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	/* We only care about DROP events on relations (indexes are relations) */
	if (access == OAT_DROP && classId == RelationRelationId && subId == 0)
	{
		ObjectAccessDrop *drop_arg = (ObjectAccessDrop *)arg;

		/* Skip internal drops */
		if ((drop_arg->dropflags & PERFORM_DELETION_INTERNAL) != 0)
			return;

		/* Check if this is one of our indexes */
		if (!tp_registry_is_registered(objectId))
			return;

		/* Cleanup shared memory BEFORE unregistering */
		tp_cleanup_index_shared_memory(objectId);
		/* tp_cleanup_index_shared_memory handles unregistering */
	}
}

/*
 * Relcache invalidation callback - clean up local state for invalidated
 * indexes This is called in all backends when an index is dropped or
 * invalidated, allowing us to clean up local DSA attachments
 */
static void
tp_relcache_callback(Datum arg, Oid relid)
{
	TpLocalIndexState *local_state;
	Relation		   rel;

	/* Check if we have any local state cached for this relation
	 * If not, we have nothing to clean up */
	local_state = tp_get_local_index_state_if_cached(relid);
	if (local_state == NULL)
		return;

	elog(DEBUG1, "Relcache callback for index %u - have local state", relid);

	/* Try to open the relation to check if it still exists */
	rel = try_relation_open(relid, NoLock);
	if (rel != NULL)
	{
		/* Index still exists - this is just a cache invalidation, not a drop
		 */
		elog(DEBUG1,
			 "Relcache callback for index %u - relation exists, skipping "
			 "cleanup",
			 relid);
		relation_close(rel, NoLock);
		return;
	}

	elog(DEBUG1,
		 "Relcache callback for index %u - relation gone, cleaning up local "
		 "state",
		 relid);

	/* Index no longer exists - clean up our local state
	 * This detaches from DSA and frees the local_state structure */
	tp_release_local_index_state(local_state);
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
 * Process exit callback - DISABLED to prevent crashes
 *
 * We originally tried to detach from DSA on process exit, but this caused
 * crashes when DROP EXTENSION destroyed the DSA before process exit.
 * Since the OS cleans up all resources on process exit anyway, and the
 * DSA system has its own cleanup mechanisms, we no longer need this.
 *
 * Keeping the code commented out for documentation purposes.
 */
#if 0
static void
tp_proc_exit(int code, Datum arg)
{
	/* Detach from the DSA if this backend attached to it */
	tp_registry_detach_dsa();
}
#endif
