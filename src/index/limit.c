/* Query LIMIT optimization shared by the planner and index scan. */
#include <postgres.h>

#include <math.h>

#include "constants.h"
#include "index/limit.h"

int tp_default_limit = TP_DEFAULT_QUERY_LIMIT;

int
tp_seed_limit_for_filter(int user_limit, double selectivity)
{
	double seeded;

	if (!tp_filtered_seed || selectivity <= 0.0 || selectivity >= 1.0)
		return user_limit;
	seeded = ceil(tp_filtered_seed_margin * (double)user_limit / selectivity);
	if (seeded > (double)TP_MAX_QUERY_LIMIT)
		seeded = TP_MAX_QUERY_LIMIT;
	return seeded > user_limit ? (int)seeded : user_limit;
}
