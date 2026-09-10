/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * planner/seed.h - Per-scan binding of the filtered-seed top-K
 */
#pragma once

#include <postgres.h>

#include <access/skey.h>

/*
 * Install the ExecutorStart hook that binds a seed to each BM25 index
 * scan in the plan (called from _PG_init).
 */
void tp_seed_hook_init(void);

/*
 * Seed bound to the scan identified by its ORDER BY ScanKey array and
 * index Oid, or -1 if none.  A miss always means "use tp_default_limit
 * and let the executor's backoff find the top-k".
 */
int tp_seed_lookup(ScanKey orderbys, Oid index_oid);
