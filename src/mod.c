#include <postgres.h>

#include <access/reloptions.h>
#include <catalog/objectaccess.h>
#include <catalog/pg_class_d.h>
#include <miscadmin.h>
#include <storage/ipc.h>
#include <storage/shmem.h>
#include <utils/guc.h>

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

/* Previous object access hook */
static object_access_hook_type prev_object_access_hook = NULL;

/* Object access hook function for cleaning up shared memory */
static void tp_object_access_hook(
		ObjectAccessType access,
		Oid				 classId,
		Oid				 objectId,
		int				 subId,
		void			*arg);

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
	elog(NOTICE, "Tapir extension _PG_init() starting GUC initialization");

	/*
	 * Define GUC parameters
	 */
	DefineCustomIntVariable(
			"tapir.index_memory_limit",
			"Per-index memory limit in MB (currently not enforced)",
			"Reserved for future use: controls the maximum memory each Tapir "
			"index "
			"can use",
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
			"tapir.default_limit",
			"Default limit for BM25 queries when no LIMIT is detected",
			"Controls the maximum number of documents to process when no "
			"LIMIT "
			"clause is present",
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
			"tapir.log_scores",
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

	elog(NOTICE, "Tapir extension GUC variables defined successfully");

	/*
	 * Install shared memory hooks (needed for registry)
	 */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook		= tp_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook		= tp_shmem_startup;

	elog(NOTICE, "Tapir shared memory hooks installed");

	/* Install object access hook for index lifecycle management */
	prev_object_access_hook = object_access_hook;
	object_access_hook		= tp_object_access_hook;

	elog(NOTICE, "Tapir extension _PG_init() completed successfully");
}

/*
 * Object access hook function for cleaning up shared memory when indexes are
 * dropped
 */
static void
tp_object_access_hook(
		ObjectAccessType access,
		Oid				 classId,
		Oid				 objectId,
		int				 subId,
		void			*arg)
{
	/* Call previous hook first if it exists */
	if (prev_object_access_hook)
		prev_object_access_hook(access, classId, objectId, subId, arg);

	/* Handle index drop events */
	if (access == OAT_DROP && classId == RelationRelationId)
	{
		/* Unregister from global registry first */
		tp_registry_unregister(objectId);
		/* Then clean up index-specific shared memory */
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
