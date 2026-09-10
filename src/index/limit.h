/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * limit.h - Query LIMIT optimization interface
 */
#pragma once

#include <postgres.h>

#include "constants.h"

/*
 * Query LIMIT Optimization
 *
 * When a query has a LIMIT clause and uses ORDER BY with a BM25 score,
 * only the top N results need to be computed.  This module holds the
 * formula that turns a user LIMIT plus a filter selectivity into the
 * scan's internal top-K, and the default used when no LIMIT is known.
 *
 * Which scan a given seed belongs to is decided in planner/seed.c, at
 * executor start; nothing here is shared between statements.
 */

/* Default limit when none detected */
extern int tp_default_limit;

/*
 * Selectivity-seeded top-K formula for filtered BM25 search.
 */
int tp_seed_limit_for_filter(int user_limit, double selectivity);
