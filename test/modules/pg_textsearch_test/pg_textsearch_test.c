#include <postgres.h>

#include <fmgr.h>
#include <miscadmin.h>
#include <utils/builtins.h>
#include <utils/injection_point.h>

#include "debug/injection.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_panic);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_segment_limit);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_reclaim_horizon_hold);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_legacy_segment);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_vacuum_total_len);

Datum
pg_textsearch_test_attach_panic(PG_FUNCTION_ARGS)
{
	TpInjectionCondition condition = {.pid = MyProcPid};
	const char			*point	   = TP_INJECTION_AFTER_SPILL_FINALIZE;

	if (!PG_ARGISNULL(0))
		point = text_to_cstring(PG_GETARG_TEXT_PP(0));

	InjectionPointAttach(
			point,
			"pg_textsearch",
			"tp_injection_panic",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_segment_limit(PG_FUNCTION_ARGS)
{
	TpInjectionSegmentCountLimit condition =
			{.pid = MyProcPid, .limit = PG_GETARG_INT32(0)};

	if (condition.limit < 1 || condition.limit > PG_UINT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("segment count limit must be between 1 and %u",
						PG_UINT16_MAX)));

	InjectionPointAttach(
			TP_INJECTION_SEGMENT_COUNT_LIMIT,
			"pg_textsearch",
			"tp_injection_segment_count_limit",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_reclaim_horizon_hold(PG_FUNCTION_ARGS)
{
	TpInjectionCondition condition = {.pid = MyProcPid};

	InjectionPointAttach(
			TP_INJECTION_RECLAIM_HORIZON,
			"pg_textsearch",
			"tp_injection_reclaim_horizon_hold",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_legacy_segment(PG_FUNCTION_ARGS)
{
	int64 total_tokens = PG_GETARG_INT64(0);
	TpInjectionTokenTotal condition;

	if (total_tokens < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("legacy segment token total must be nonnegative")));

	condition.pid		  = MyProcPid;
	condition.total_tokens = (uint64)total_tokens;
	InjectionPointAttach(
			TP_INJECTION_LEGACY_SEGMENT,
			"pg_textsearch",
			"tp_injection_legacy_segment",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_vacuum_total_len(PG_FUNCTION_ARGS)
{
	int64 total_tokens = PG_GETARG_INT64(0);
	TpInjectionTokenTotal condition;

	if (total_tokens < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("VACUUM token total must be nonnegative")));

	condition.pid		  = MyProcPid;
	condition.total_tokens = (uint64)total_tokens;
	InjectionPointAttach(
			TP_INJECTION_VACUUM_TOTAL_LEN,
			"pg_textsearch",
			"tp_injection_vacuum_total_len",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}
