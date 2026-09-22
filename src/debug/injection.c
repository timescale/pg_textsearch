/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */

#include <postgres.h>

#include "access/xlog.h"
#include "debug/injection.h"
#include "miscadmin.h"

static uint32 injected_segment_count_limit	= PG_UINT16_MAX;
static bool	  injected_reclaim_horizon_held = false;

#if PG_VERSION_NUM >= 180000
#define TP_INJECTION_CALLBACK_ARGS \
	const char *name, const void *private_data, void *arg
#else
#define TP_INJECTION_CALLBACK_ARGS const char *name, const void *private_data
#endif

#ifdef USE_INJECTION_POINTS
extern PGDLLEXPORT void tp_injection_panic(TP_INJECTION_CALLBACK_ARGS);
extern PGDLLEXPORT void
		tp_injection_segment_count_limit(TP_INJECTION_CALLBACK_ARGS);
extern PGDLLEXPORT void
		tp_injection_reclaim_horizon_hold(TP_INJECTION_CALLBACK_ARGS);
#endif

uint32
tp_injected_segment_count_limit(void)
{
	injected_segment_count_limit = PG_UINT16_MAX;
	TP_INJECTION_POINT(TP_INJECTION_SEGMENT_COUNT_LIMIT);
	return injected_segment_count_limit;
}

/*
 * Report whether a test is pinning the deferred-reclaim horizon so no
 * parked page is recyclable.
 */
bool
tp_injected_reclaim_horizon_held(void)
{
	injected_reclaim_horizon_held = false;
	TP_INJECTION_POINT(TP_INJECTION_RECLAIM_HORIZON);
	return injected_reclaim_horizon_held;
}

#ifdef USE_INJECTION_POINTS
void
tp_injection_panic(TP_INJECTION_CALLBACK_ARGS)
{
	const TpInjectionCondition *condition = private_data;

#if PG_VERSION_NUM >= 180000
	(void)arg;
#endif
	if (condition->pid != MyProcPid)
		return;

	XLogFlush(GetXLogInsertRecPtr());
	elog(PANIC, "panic triggered for injection point %s", name);
}

void
tp_injection_segment_count_limit(TP_INJECTION_CALLBACK_ARGS)
{
	const TpInjectionSegmentCountLimit *condition = private_data;

#if PG_VERSION_NUM >= 180000
	(void)arg;
#endif
	if (condition->pid != MyProcPid)
		return;
	if (condition->limit < 1 || condition->limit > PG_UINT16_MAX)
		elog(ERROR,
			 "invalid segment count limit for injection point %s",
			 name);

	injected_segment_count_limit = (uint32)condition->limit;
}

void
tp_injection_reclaim_horizon_hold(TP_INJECTION_CALLBACK_ARGS)
{
	const TpInjectionCondition *condition = private_data;

#if PG_VERSION_NUM >= 180000
	(void)arg;
#endif
	(void)name;
	if (condition->pid != MyProcPid)
		return;

	injected_reclaim_horizon_held = true;
}
#endif
