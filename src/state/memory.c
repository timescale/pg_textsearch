/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memory.c - Memory usage visibility functions
 */
#include <postgres.h>

#include <funcapi.h>
#include <utils/builtins.h>

#include "state/memory.h"
#include "state/registry.h"

PG_FUNCTION_INFO_V1(tp_memory_usage);

Datum
tp_memory_usage(PG_FUNCTION_ARGS)
{
	TupleDesc tupdesc;
	Datum	  values[5];
	bool	  nulls[5];
	HeapTuple tuple;
	uint64	  total_bytes;
	int64	  max_kb;
	uint64	  max_bytes;

	/* Build tuple descriptor */
	tupdesc = CreateTemplateTupleDesc(5);
	TupleDescInitEntry(tupdesc, 1, "total_dsa_bytes", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, 2, "total_dsa_mb", FLOAT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 3, "max_memory_bytes", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, 4, "max_memory_mb", FLOAT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 5, "usage_pct", FLOAT4OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	memset(nulls, 0, sizeof(nulls));

	/* Read current DSA usage */
	tp_registry_update_dsa_counter();
	total_bytes = tp_registry_get_total_dsa_bytes();

	values[0] = Int64GetDatum((int64)total_bytes);
	values[1] = Float4GetDatum((float4)total_bytes / (1024.0f * 1024.0f));

	/* Read max_memory GUC */
	max_kb = (int64)tp_max_memory;

	if (max_kb > 0)
	{
		max_bytes = (uint64)max_kb * 1024ULL;
		values[2] = Int64GetDatum((int64)max_bytes);
		values[3] = Float4GetDatum((float4)max_bytes / (1024.0f * 1024.0f));
		values[4] = Float4GetDatum(
				(float4)total_bytes / (float4)max_bytes * 100.0f);
	}
	else
	{
		nulls[2] = true;
		nulls[3] = true;
		nulls[4] = true;
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}
