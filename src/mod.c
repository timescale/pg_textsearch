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

#include "constants.h"
#include "memtable.h"
#include "posting.h"
#include "registry.h"
#include "state.h"

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(.name = "pg_textsearch", .version = "0.0.3-dev");
#else
PG_MODULE_MAGIC;
#endif

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
			"pg_textsearch.index_memory_limit",
			"Per-index memory limit in MB",
			"Controls the maximum memory each pg_textsearch index can use",
			&tp_index_memory_limit,
			TP_DEFAULT_INDEX_MEMORY_LIMIT, /* default 64MB */
			1,							   /* min 1MB */
			TP_MAX_INDEX_MEMORY_LIMIT,	   /* max 512MB */
			PGC_SUSET,					   /* Superuser can change with SET */
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
 */
static void
tp_xact_callback(XactEvent event, void *arg __attribute__((unused)))
{
	/* We only care about transaction commit and abort */
	switch (event)
	{
	case XACT_EVENT_COMMIT:
	case XACT_EVENT_ABORT:
	case XACT_EVENT_PARALLEL_COMMIT:
	case XACT_EVENT_PARALLEL_ABORT:
		/* Release all index locks held by this backend */
		tp_release_all_index_locks();
		break;

	case XACT_EVENT_PRE_COMMIT:
	case XACT_EVENT_PARALLEL_PRE_COMMIT:
	case XACT_EVENT_PRE_PREPARE:
	case XACT_EVENT_PREPARE:
		/* Nothing to do for these events */
		break;
	}
}
