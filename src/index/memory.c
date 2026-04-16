/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memory.c - Memory usage visibility functions
 */
#include <postgres.h>

#include <funcapi.h>
#include <utils/builtins.h>

#include "index/memory.h"
#include "index/registry.h"

PG_FUNCTION_INFO_V1(tp_memory_usage);

Datum
tp_memory_usage(PG_FUNCTION_ARGS)
{
	TupleDesc tupdesc;
	Datum	  values[7];
	bool	  nulls[7];
	HeapTuple tuple;
	uint64	  dsa_bytes;
	uint64	  est_bytes;
	uint64	  counter_bytes;

	/* Build tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(7);
	TupleDescInitEntry(tupdesc, 1, "dsa_total_bytes", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, 2, "dsa_total_mb", FLOAT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 3, "estimated_bytes", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, 4, "estimated_mb", FLOAT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 5, "counter_bytes", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, 6, "memory_limit_mb", FLOAT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 7, "usage_pct", FLOAT4OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	memset(nulls, 0, sizeof(nulls));

	/* DSA segment reservation */
	tp_registry_update_dsa_counter();
	dsa_bytes = tp_registry_get_total_dsa_bytes();

	values[0] = Int64GetDatum((int64)dsa_bytes);
	values[1] = Float4GetDatum((float4)dsa_bytes / (1024.0f * 1024.0f));

	/* Estimated memtable memory (authoritative registry scan) */
	est_bytes = tp_estimate_total_memtable_bytes();

	values[2] = Int64GetDatum((int64)est_bytes);
	values[3] = Float4GetDatum((float4)est_bytes / (1024.0f * 1024.0f));

	/* Atomic counter (incrementally maintained, O(1)) */
	counter_bytes = tp_get_estimated_total_bytes();
	values[4]	  = Int64GetDatum((int64)counter_bytes);

	/* Memory limit */
	if (tp_memory_limit > 0)
	{
		uint64 limit_bytes = tp_hard_limit_bytes();
		values[5] = Float4GetDatum((float4)limit_bytes / (1024.0f * 1024.0f));
		values[6] = Float4GetDatum(
				(float4)dsa_bytes / (float4)limit_bytes * 100.0f);
	}
	else
	{
		nulls[5] = true;
		nulls[6] = true;
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
