/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */

#include <postgres.h>

#include "access/xlog.h"
#include "debug/injection.h"
#include "miscadmin.h"
#include "storage/proc.h"

static uint32 injected_segment_count_limit	= PG_UINT16_MAX;
static bool	  injected_reclaim_horizon_held = false;
static bool	  injected_legacy_segment		= false;
static uint64 injected_legacy_total_tokens	= 0;
static bool	  injected_vacuum_total_len		= false;
static uint64 injected_vacuum_total_tokens	= 0;

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
extern PGDLLEXPORT void
		tp_injection_legacy_segment(TP_INJECTION_CALLBACK_ARGS);
extern PGDLLEXPORT void
		tp_injection_vacuum_total_len(TP_INJECTION_CALLBACK_ARGS);
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

bool
tp_injected_legacy_segment(uint64 *total_tokens)
{
	injected_legacy_segment		 = false;
	injected_legacy_total_tokens = 0;
	TP_INJECTION_POINT(TP_INJECTION_LEGACY_SEGMENT);
	if (injected_legacy_segment)
		*total_tokens = injected_legacy_total_tokens;
	return injected_legacy_segment;
}

uint64
tp_injected_vacuum_total_len(uint64 total_tokens)
{
	injected_vacuum_total_len	 = false;
	injected_vacuum_total_tokens = 0;
	TP_INJECTION_POINT(TP_INJECTION_VACUUM_TOTAL_LEN);
	return injected_vacuum_total_len ? injected_vacuum_total_tokens
									 : total_tokens;
}

#ifdef USE_INJECTION_POINTS
/*
 * Report whether the attaching backend is this process or, when a
 * parallel worker fires the point, its lock group leader.
 */
static bool
tp_injection_pid_matches(int pid)
{
	if (pid == MyProcPid)
		return true;
	if (MyProc != NULL && MyProc->lockGroupLeader != NULL)
		return pid == MyProc->lockGroupLeader->pid;
	return false;
}

void
tp_injection_panic(TP_INJECTION_CALLBACK_ARGS)
{
	const TpInjectionCondition *condition = private_data;

#if PG_VERSION_NUM >= 180000
	(void)arg;
#endif
	if (!tp_injection_pid_matches(condition->pid))
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
	if (!tp_injection_pid_matches(condition->pid))
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
	if (!tp_injection_pid_matches(condition->pid))
		return;

	injected_reclaim_horizon_held = true;
}

void
tp_injection_legacy_segment(TP_INJECTION_CALLBACK_ARGS)
{
	const TpInjectionTokenTotal *condition = private_data;

#if PG_VERSION_NUM >= 180000
	(void)arg;
#endif
	(void)name;
	if (!tp_injection_pid_matches(condition->pid))
		return;

	injected_legacy_segment		 = true;
	injected_legacy_total_tokens = condition->total_tokens;
}

void
tp_injection_vacuum_total_len(TP_INJECTION_CALLBACK_ARGS)
{
	const TpInjectionTokenTotal *condition = private_data;

#if PG_VERSION_NUM >= 180000
	(void)arg;
#endif
	(void)name;
	if (!tp_injection_pid_matches(condition->pid))
		return;

	injected_vacuum_total_len	 = true;
	injected_vacuum_total_tokens = condition->total_tokens;
}
#endif
