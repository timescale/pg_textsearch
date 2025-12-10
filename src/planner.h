#pragma once

#include <postgres.h>

#include <nodes/params.h>
#include <nodes/parsenodes.h>
#include <nodes/plannodes.h>

/*
 * Planner hook for implicit index resolution
 *
 * When a query uses the <@> operator with an unresolved tpquery (index_oid
 * is InvalidOid), this hook identifies the column being scored and finds
 * a suitable BM25 index to use.
 */

/* Initialize planner hook (called from _PG_init) */
void tp_planner_hook_init(void);

/* Main planner hook function */
PlannedStmt *tp_planner_hook(
		Query		 *parse,
		const char	 *query_string,
		int			  cursorOptions,
		ParamListInfo boundParams);
