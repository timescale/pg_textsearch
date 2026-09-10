/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * planner/hooks.h - Planner hooks for BM25 query optimization
 */
#pragma once

#include <postgres.h>

/*
 * Initialize planner hooks (called from _PG_init).
 * Registers post_parse_analyze_hook and planner_hook.
 */
void tp_planner_hook_init(void);

/*
 * Oid of the bm25 access method, or InvalidOid if the extension is not
 * installed.  Resolved lazily and cached per backend, so callers on hot
 * paths must still avoid asking about non-BM25 statements.
 */
Oid tp_get_bm25_am_oid(void);
