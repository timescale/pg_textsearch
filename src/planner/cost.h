/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * planner/cost.h - Cost estimation for BM25 index scans
 */
#pragma once

#include <postgres.h>

#include <nodes/pathnodes.h>
#include <optimizer/optimizer.h>

/*
 * Estimate cost of BM25 index scan
 */
void tp_costestimate(
		PlannerInfo *root,
		IndexPath	*path,
		double		 loop_count,
		Cost		*indexStartupCost,
		Cost		*indexTotalCost,
		Selectivity *indexSelectivity,
		double		*indexCorrelation,
		double		*indexPages);
