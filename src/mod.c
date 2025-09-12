#include <postgres.h>

#include <access/reloptions.h>
#include <catalog/objectaccess.h>
#include <catalog/pg_class_d.h>
#include <miscadmin.h>
#include <utils/guc.h>

#include "constants.h"
#include "index.h"
#include "memtable.h"
#include "mod.h"
#include "posting.h"

PG_MODULE_MAGIC;

/* Relation options for Tapir indexes */
relopt_kind tp_relopt_kind;

/* Previous object access hook */
static object_access_hook_type prev_object_access_hook = NULL;

/* Object access hook function for cleaning up shared memory */
static void tp_object_access_hook(
		ObjectAccessType access,
		Oid				 classId,
		Oid				 objectId,
		int				 subId,
		void			*arg);

/*
 * Extension entry point - called when the extension is loaded
 */
void
_PG_init(void)
{
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
	 * Install object access hook for cleanup when indexes are dropped
	 */
	prev_object_access_hook = object_access_hook;
	object_access_hook		= tp_object_access_hook;

	elog(DEBUG1,
		 "Tapir extension loaded with index_limit=%dMB (currently not "
		 "enforced)",
		 tp_index_memory_limit);
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
		/* Check if this is a Tapir index by looking up in our registry */
		tp_cleanup_index_shared_memory(objectId);
	}
}
