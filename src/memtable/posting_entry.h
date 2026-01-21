/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable/posting_entry.h - Common posting entry type
 *
 * This type is shared between the DSA-based shared memtable and the
 * palloc-based local memtable used in parallel builds.
 */
#pragma once

#include <postgres.h>

#include <storage/itemptr.h>

/*
 * Individual document occurrence within a posting list.
 * Doc IDs are assigned at segment write time via docmap lookup.
 */
typedef struct TpPostingEntry
{
	ItemPointerData ctid;	   /* Document heap tuple ID */
	int32			frequency; /* Term frequency in document */
} TpPostingEntry;
