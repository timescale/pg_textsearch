/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * posting_entry.h — common posting entry type.
 *
 * Shared between the in-memory memtable cache (DSA-allocated
 * posting lists) and the palloc-based local memtable used in
 * parallel builds.  See docs/memtable_cache.md.
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
