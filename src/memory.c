/*-------------------------------------------------------------------------
 *
 * memory.c
 *	  Memory accounting and management for pg_textsearch
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
 * Allocate memory with tracking
 *
 * Allocates memory from DSA and updates the memory usage counter.
 * Returns InvalidDsaPointer if allocation would exceed limit.
 */
dsa_pointer
tp_dsa_allocate(dsa_area *dsa, TpMemoryUsage *memory_usage, Size size)
{
	dsa_pointer result;
	Size		new_usage;

	Assert(dsa != NULL);
	Assert(memory_usage != NULL);

	/* Check if allocation would exceed limit */
	new_usage = memory_usage->memory_used + size;
	if (new_usage > tp_get_memory_limit())
	{
		return InvalidDsaPointer;
	}

	/* Perform allocation */
	result = dsa_allocate(dsa, size);
	if (!DsaPointerIsValid(result))
	{
		return InvalidDsaPointer;
	}

	/* Update memory tracking */
	memory_usage->memory_used = new_usage;

	return result;
}

/*
 * Free memory with tracking
 *
 * Frees memory in DSA and updates the memory usage counter.
 * The size parameter must match the original allocation size.
 */
void
tp_dsa_free(
		dsa_area *dsa, TpMemoryUsage *memory_usage, dsa_pointer ptr, Size size)
{
	Assert(dsa != NULL);
	Assert(memory_usage != NULL);

	if (!DsaPointerIsValid(ptr))
		return;

	/* Free memory in DSA */
	dsa_free(dsa, ptr);

	/* Update memory tracking */
	Assert(memory_usage->memory_used >= size);
	memory_usage->memory_used -= size;
}

/*
 * Get current memory usage
 */
Size
tp_get_memory_usage(TpMemoryUsage *memory_usage)
{
	Assert(memory_usage != NULL);

	return memory_usage->memory_used;
}

/*
 * Check if allocation would exceed limit
 */
bool
tp_check_memory_limit(TpMemoryUsage *memory_usage, Size additional_bytes)
{
	Assert(memory_usage != NULL);

	return (memory_usage->memory_used + additional_bytes <=
			tp_get_memory_limit());
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
tp_report_memory_limit_exceeded(TpMemoryUsage *memory_usage)
{
	Assert(memory_usage != NULL);

	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
			 errmsg("pg_textsearch index memory limit exceeded"),
			 errdetail(
					 "Current usage: %zu bytes, limit: %zu bytes",
					 tp_get_memory_usage(memory_usage),
					 tp_get_memory_limit()),
			 errhint("Increase pg_textsearch.index_memory_limit or "
					 "reduce the amount of data being indexed.")));
}
