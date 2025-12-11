/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * limit.h - Query LIMIT optimization interface
 */
#pragma once

#include <postgres.h>

#include <nodes/pathnodes.h>
#include <utils/rel.h>

#include "constants.h"

/*
 * Query LIMIT Optimization
 *
 * This module handles query LIMIT pushdown optimization for Tapir indexes.
 * When a query has a LIMIT clause and uses ORDER BY with a BM25 score,
 * we can optimize by only computing the top N results instead of all results.
 */

/*
 * Simple per-backend structure to track the current query's limit
 * Replaces the hash table approach for better simplicity and performance
 */
typedef struct TpCurrentLimit
{
	Oid	 index_oid; /* Index OID for which this limit applies */
	int	 limit;		/* LIMIT value from query */
	bool is_valid;	/* Whether this data is current and valid */
} TpCurrentLimit;

/* Default limit when none detected */
extern int tp_default_limit;

/*
 * Query limit tracking functions
 */
void tp_store_query_limit(Oid index_oid, int limit);
int	 tp_get_query_limit(Relation index_rel);
void tp_cleanup_query_limits(void);

/*
 * LIMIT pushdown analysis for cost estimation
 */
bool tp_can_pushdown_limit(PlannerInfo *root, IndexPath *path, int limit);

/*
 * Module initialization
 */
void tp_limits_init(void);
