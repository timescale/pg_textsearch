/*-------------------------------------------------------------------------
 *
 * memory.c
 *	  Memory management for pg_textsearch
 *
 * IDENTIFICATION
 *	  src/memory.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <miscadmin.h>
#include <utils/dsa.h>

#include "memory.h"

/* External GUC variable */
extern int tp_index_memory_limit;

/*
 * Get current DSA memory usage using dsa_get_total_size().
 *
 * This returns the total size of all DSA segments allocated for this area.
 * It captures all memory including dshash internal structures, bucket
 * arrays, and free space within segments.
 */
Size
tp_get_dsa_memory_usage(dsa_area *dsa)
{
	if (dsa == NULL)
		return 0;

	return dsa_get_total_size(dsa);
}

/*
 * Get configured memory limit in bytes
 */
Size
tp_get_memory_limit(void)
{
	/* Convert megabytes to bytes */
	return (Size)tp_index_memory_limit * 1024 * 1024;
}

/*
 * Report memory limit exceeded error
 *
 * Centralized error reporting for memory limit violations.
 */
void
tp_report_memory_limit_exceeded(dsa_area *dsa)
{
	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
			 errmsg("pg_textsearch index memory limit exceeded"),
			 errdetail(
					 "Current DSA usage: %zu bytes, limit: %zu bytes",
					 tp_get_dsa_memory_usage(dsa),
					 tp_get_memory_limit()),
			 errhint("Increase pg_textsearch.index_memory_limit or "
					 "reduce the amount of data being indexed.")));
}
