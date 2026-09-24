/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

#include <utils/injection_point.h>

#define TP_INJECTION_AFTER_SPILL_FINALIZE "pg-textsearch-after-spill-finalize"
#define TP_INJECTION_SEGMENT_COUNT_LIMIT  "pg-textsearch-segment-count-limit"

#if PG_VERSION_NUM >= 180000
#define TP_INJECTION_POINT(name) INJECTION_POINT(name, NULL)
#else
#define TP_INJECTION_POINT(name) INJECTION_POINT(name)
#endif

typedef struct TpInjectionCondition
{
	int pid;
} TpInjectionCondition;

typedef struct TpInjectionSegmentCountLimit
{
	int	  pid;
	int32 limit;
} TpInjectionSegmentCountLimit;

extern uint32 tp_segment_count_limit(void);
