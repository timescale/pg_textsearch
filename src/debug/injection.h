/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

#define TP_INJECTION_AFTER_SPILL_FINALIZE "pg-textsearch-after-spill-finalize"
#define TP_INJECTION_SEGMENT_COUNT_LIMIT  "pg-textsearch-segment-count-limit"

#if PG_VERSION_NUM >= 180000
#define TP_INJECTION_POINT(name) INJECTION_POINT(name, NULL)
#define TP_INJECTION_CALLBACK_ARGS \
	const char *name, const void *private_data, void *arg
#else
#define TP_INJECTION_POINT(name)   INJECTION_POINT(name)
#define TP_INJECTION_CALLBACK_ARGS const char *name, const void *private_data
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
extern void	  tp_injection_point_after_spill_finalize(void);

#ifdef USE_INJECTION_POINTS
extern PGDLLEXPORT void tp_injection_panic(TP_INJECTION_CALLBACK_ARGS);
extern PGDLLEXPORT void
		tp_injection_segment_count_limit(TP_INJECTION_CALLBACK_ARGS);
#endif
