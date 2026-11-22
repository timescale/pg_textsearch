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

/* Whether the installed SQL lets the planner carry a row's partition. */
bool tp_partition_scoring_available(void);
