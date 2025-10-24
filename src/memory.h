/*-------------------------------------------------------------------------
 *
 * memory.h
 *	  Memory accounting and management for pg_textsearch
 *
 * IDENTIFICATION
 *	  src/memory.h
 *
 *-------------------------------------------------------------------------
 */
#pragma once

#include <postgres.h>

#include <port/atomics.h>
#include <utils/dsa.h>

/*
 * Memory usage tracking structure
 *
 * This structure encapsulates memory usage counters and can be passed
 * to low-level allocation functions without creating circular dependencies.
 * It is embedded inline in TpSharedIndexState.
 *
 * Uses atomic operations for lock-free memory visibility across processes.
 */
typedef struct TpMemoryUsage
{
	pg_atomic_uint64 memory_used; /* Current memory usage in bytes */
} TpMemoryUsage;

/*
 * Memory accounting functions
 *
 * These wrapper functions track memory allocations and deallocations
 * to enforce per-index memory limits.
 *
 * Low-level functions take (dsa_area *, TpMemoryUsage *) to avoid
 * circular dependencies with state.h.
 */

/* Initialize memory usage tracking (must be called before first use) */
extern void tp_init_memory_usage(TpMemoryUsage *memory_usage);

/* Allocate memory with tracking */
extern dsa_pointer
tp_dsa_allocate(dsa_area *dsa, TpMemoryUsage *memory_usage, Size size);

/* Free memory with tracking */
extern void tp_dsa_free(
		dsa_area	  *dsa,
		TpMemoryUsage *memory_usage,
		dsa_pointer	   ptr,
		Size		   size);

/* Get current memory usage */
extern Size tp_get_memory_usage(TpMemoryUsage *memory_usage);

/* Get configured memory limit in bytes */
extern Size tp_get_memory_limit(void);

/* Report memory limit exceeded error */
extern void tp_report_memory_limit_exceeded(TpMemoryUsage *memory_usage)
		pg_attribute_noreturn();
