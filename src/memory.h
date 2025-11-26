/*-------------------------------------------------------------------------
 *
 * memory.h
 *	  Memory management for pg_textsearch
 *
 * IDENTIFICATION
 *	  src/memory.h
 *
 *-------------------------------------------------------------------------
 */
#pragma once

#include <postgres.h>

#include <utils/dsa.h>

/*
 * Get current DSA memory usage using dsa_get_total_size().
 *
 * This returns the total size of all DSA segments, which includes all
 * allocations (strings, posting lists, dshash internal structures, etc.)
 * plus some internal overhead and free space within segments.
 *
 * This is more accurate than manual tracking because it captures all DSA
 * usage including dshash internal allocations that we don't track directly.
 */
extern Size tp_get_dsa_memory_usage(dsa_area *dsa);

/* Get configured memory limit in bytes */
extern Size tp_get_memory_limit(void);

/* Report memory limit exceeded error */
extern void tp_report_memory_limit_exceeded(dsa_area *dsa);
