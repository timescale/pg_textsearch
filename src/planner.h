/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

/*
 * Parse analysis hook for implicit index resolution
 *
 * When a query uses the <@> operator with an unresolved tpquery (index_oid
 * is InvalidOid), this hook identifies the column being scored and finds
 * a suitable BM25 index to use. Uses post_parse_analyze_hook to ensure
 * transformation happens before the planner generates paths.
 */

/* Initialize parse analysis hook (called from _PG_init) */
void tp_planner_hook_init(void);
