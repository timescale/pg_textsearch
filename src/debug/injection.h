/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 */
#pragma once

#include <postgres.h>

#include <utils/injection_point.h>

#define TP_INJECTION_AFTER_SPILL_FINALIZE "pg-textsearch-after-spill-finalize"
#define TP_INJECTION_SEGMENT_COUNT_LIMIT  "pg-textsearch-segment-count-limit"
#define TP_INJECTION_RECLAIM_HORIZON	  "pg-textsearch-reclaim-horizon"
#define TP_INJECTION_LEGACY_SEGMENT		  "pg-textsearch-legacy-segment"
#define TP_INJECTION_VACUUM_TOTAL_LEN	  "pg-textsearch-vacuum-total-len"
#define TP_INJECTION_VACUUM_MEMTABLE_RECLAIM \
	"pg-textsearch-vacuum-memtable-reclaim"

/*
 * Points that hold a backend at a chosen step of the spill and
 * compaction sequence so another session can race it.  Tests attach
 * the "wait" action from the injection_points module and release the
 * backend with injection_points_wakeup().
 */
#define TP_INJECTION_SPILL_BEFORE_FINALIZE \
	"pg-textsearch-spill-before-finalize"
#define TP_INJECTION_BEFORE_COMPACTION_PUBLISH \
	"pg-textsearch-before-compaction-publish"
#define TP_INJECTION_AFTER_COMPACTION_PUBLISH \
	"pg-textsearch-after-compaction-publish"
#define TP_INJECTION_COMPACTION_AFTER_SELECT \
	"pg-textsearch-compaction-after-select"
#define TP_INJECTION_COMPACTION_SOURCE_ESTIMATE \
	"pg-textsearch-compaction-source-estimate"
#define TP_INJECTION_COMPACTION_BEFORE_PUBLISH \
	"pg-textsearch-compaction-before-publish"
#define TP_INJECTION_COMPACTION_AFTER_RESTAMP \
	"pg-textsearch-compaction-after-restamp"
#define TP_INJECTION_COMPACTION_ALLOC_OUTPUT_DATA \
	"pg-textsearch-compaction-alloc-output-data"
#define TP_INJECTION_COMPACTION_ALLOC_PAGE_INDEX \
	"pg-textsearch-compaction-alloc-page-index"
#define TP_INJECTION_COMPACTION_ALLOC_TOMBSTONE \
	"pg-textsearch-compaction-alloc-tombstone"
#define TP_INJECTION_MEMTABLE_EXTEND "pg-textsearch-memtable-extend"
#define TP_INJECTION_INDEX_LOCK_EXCLUSIVE_WAITER \
	"pg-textsearch-index-lock-exclusive-waiter"

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

typedef struct TpInjectionTokenTotal
{
	int	   pid;
	uint64 total_tokens;
} TpInjectionTokenTotal;

/*
 * tp_injected_* report a condition that only an attached injection
 * point can produce; without one they return the natural value.
 */
extern uint32 tp_injected_segment_count_limit(void);
extern bool	  tp_injected_reclaim_horizon_held(void);
extern bool	  tp_injected_legacy_segment(uint64 *total_tokens);
extern uint64 tp_injected_vacuum_total_len(uint64 total_tokens);
