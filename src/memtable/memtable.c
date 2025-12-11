/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */

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
