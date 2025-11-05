/*-------------------------------------------------------------------------
 *
 * memtable.c
 *	  Tapir BM25 in-memory table implementation
 *
 * IDENTIFICATION
 *	  src/memtable.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <utils/memutils.h>

#include "memtable.h"

/* GUC variable for memory limit (currently not enforced) */
int tp_index_memory_limit = 16; /* 16MB default */
