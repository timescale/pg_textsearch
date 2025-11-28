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

/* Get current DSA memory usage */
extern Size tp_get_dsa_memory_usage(dsa_area *dsa);

/* Get configured memory limit in bytes */
extern Size tp_get_memory_limit(void);

/* Report memory limit exceeded error */
extern void tp_report_memory_limit_exceeded(dsa_area *dsa);
