#pragma once

#include <postgres.h>

#include <nodes/pathnodes.h>
#include <utils/hsearch.h>
#include <utils/rel.h>

#include "constants.h"

/*
 * Tapir Query LIMIT Optimization
 *
 * This module handles query LIMIT pushdown optimization for Tapir indexes.
 * When a query has a LIMIT clause and uses ORDER BY with a BM25 score,
 * we can optimize by only computing the top N results instead of all results.
 */

/*
 * Query limit entry for the per-backend hash table
 */
typedef struct TpQueryLimitEntry
{
	Oid index_oid; /* Hash key - index OID */
	int limit;	   /* LIMIT value from query */
} TpQueryLimitEntry;

/*
 * External variables
 */
extern HTAB
		  *tp_query_limits_hash; /* Per-backend hash table for query limits */
extern int tp_default_limit;	 /* Default limit when none detected */

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
