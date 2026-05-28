/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable.c — in-memory memtable cache implementation
 *
 * Currently just a translation unit anchor: the cache primitives
 * live in stringtable.c (string interning + term->posting-list
 * map) and posting.c (posting list mutation + doclength table).
 * Kept as a separate file so future top-level cache helpers
 * (e.g., tp_cache_clear, eviction) have an obvious home.
 *
 * See docs/memtable_cache.md.
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "memtable/memtable.h"
