#include "postgres.h"
#include "access/reloptions.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "constants.h"
#include "index.h"
#include "constants.h"
#include "mod.h"
#include "memtable.h"

PG_MODULE_MAGIC;

/* Relation options for Tp indexes */
relopt_kind tp_relopt_kind;

/*
 * Extension entry point - called when the extension is loaded
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.
	 */
	if (!process_shared_preload_libraries_in_progress)
	{
		ereport(FATAL,
			(errmsg("extension \"%s\" must be preloaded", TP_EXTENSION_NAME),
			 errhint("Please preload the tapir library via shared_preload_libraries.\n\n"
					 "This can be done by editing the postgres config file \n"
					 "and adding 'tapir' to the list in the shared_preload_libraries "
					 "config.\n"
					 "	# Modify postgresql.conf:\n	shared_preload_libraries = "
					 "'tapir'\n\n"
					 "Another way to do this, if not preloading other libraries, is with the "
					 "command:\n"
					 "	echo \"shared_preload_libraries = 'tapir'\" >> "
					 "/path/to/config/file \n\n"
					 "(Will require a database restart.)\n\n")));
	}

	/*
	 * Define GUC parameters
	 */
	DefineCustomIntVariable(
							"tapir.shared_memory_size",
							"Size of Tapir shared memory in MB",
							"Controls the amount of shared memory allocated for Tapir memtables",
							&tp_shared_memory_size,
							TP_DEFAULT_SHARED_MEMORY_SIZE, /* default 64MB */
							1,	/* min 1MB */
							TP_MAX_SHARED_MEMORY_SIZE,	/* max 1GB */
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable(
							"tapir.default_limit",
							"Default limit for BM25 queries when no LIMIT is detected",
							"Controls the maximum number of documents to process when no LIMIT clause is present",
							&tp_default_limit,
							TP_DEFAULT_QUERY_LIMIT,	/* default 1000 */
							1,		/* min 1 */
							TP_MAX_QUERY_LIMIT, /* max 100k */
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	/*
	 * Initialize index access method options
	 */
	tp_relopt_kind = add_reloption_kind();
	add_string_reloption(tp_relopt_kind,
						 "text_config",
						 "Text search configuration for Tapir index",
						 NULL,
						 NULL,
						 NoLock);
	add_real_reloption(
					   tp_relopt_kind, "k1", "BM25 k1 parameter", TP_DEFAULT_K1, 0.1, 10.0, NoLock);
	add_real_reloption(
					   tp_relopt_kind, "b", "BM25 b parameter", TP_DEFAULT_B, 0.0, 1.0, NoLock);

	/*
	 * Install memtable hooks for shared memory management
	 */
	tp_init_memtable_hooks();

	elog(DEBUG1,
		 "Tapir extension loaded with shared_memory_size=%dMB",
		 tp_shared_memory_size);
}
