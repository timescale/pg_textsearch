/*-------------------------------------------------------------------------
 *
 * memtable.c
 *	  Simplified memtable management without locks
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
int tp_index_memory_limit = 64; /* 64MB default */
