#include "debug/injection.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postgres.h"
#include "utils/injection_point.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_panic);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_segment_limit);

Datum
pg_textsearch_test_attach_panic(PG_FUNCTION_ARGS)
{
	TpInjectionCondition condition = {.pid = MyProcPid};

	InjectionPointAttach(
			TP_INJECTION_AFTER_SPILL_FINALIZE,
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
