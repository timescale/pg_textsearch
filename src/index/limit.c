/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * limit.c - Query LIMIT optimization
 *
 * Implements LIMIT pushdown for BM25 queries. When queries have LIMIT
 * clauses with ORDER BY BM25 scores, we compute only the top N results.
 * This module is the seed formula and the default limit; the seed is
 * bound to a particular scan in planner/seed.c.
 */
#include <postgres.h>

#include <math.h>
#include <utils/guc.h>

#include "index/limit.h"

/*
 * Default limit when no LIMIT clause is detected - prevents
 * unbounded result sets that could consume excessive memory
 */
int tp_default_limit = TP_DEFAULT_QUERY_LIMIT;

/*
 * Seed the internal top-K from the estimated selectivity of the filter
 * ("facet") that the executor applies as a Filter above the BM25 index
 * scan.
 *
 * A filtered top-k query (WHERE <filter> ORDER BY <score> LIMIT k) is
 * planned as a BM25 top-k scan with <filter> applied above it.  If the
 * scan only produces its top k rows by score, few may satisfy the
 * Filter, forcing the executor to re-drive the scan with an
 * exponentially growing internal limit (backoff) until k rows survive --
 * and each re-drive re-scores from scratch.
 *
 * To surface k Filter-matching rows we expect to score ~k/s, where s is
 * the filter selectivity.  Seeding the internal top-K to
 * ceil(margin * k / s) up front lets a single scoring pass usually
 * suffice; the existing backoff remains the correctness safety net when
 * the estimate under-shoots.  The seed only changes scan depth, never
 * which rows win, so results are identical to the un-seeded plan.
 *
 * Returns the (possibly seeded) limit: always >= user_limit and capped
 * at TP_MAX_QUERY_LIMIT.  With seeding disabled, no filter, or a
 * degenerate selectivity estimate, returns user_limit unchanged.
 */
int
tp_seed_limit_for_filter(int user_limit, double selectivity)
{
	double seeded;

	if (!tp_filtered_seed)
		return user_limit;

	/* Only seed for a genuinely selective, non-degenerate filter. */
	if (selectivity <= 0.0 || selectivity >= 1.0)
		return user_limit;

	seeded = ceil(tp_filtered_seed_margin * (double)user_limit / selectivity);
	if (seeded > (double)TP_MAX_QUERY_LIMIT)
		seeded = (double)TP_MAX_QUERY_LIMIT;

	if (seeded <= (double)user_limit)
		return user_limit;

	return (int)seeded;
}
