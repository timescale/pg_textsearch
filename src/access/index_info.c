/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * index_info.c - Inspection of BM25 index header facts, for the index owner
 *                and roles with MAINTAIN on its table
 */
#include <postgres.h>

#include <access/htup_details.h>
#include <access/relation.h>
#include <funcapi.h>

#include "access/am.h"
#include "access/index_info.h"
#include "index/metapage.h"

/*
 * Absent or undecodable fields become NULL rather than fabricated zeroes;
 * every field is NULL when the index has no header yet.
 */
Datum
tp_index_header_info_datum(
		FunctionCallInfo fcinfo, const TpIndexHeaderInfo *info)
{
	TupleDesc tupdesc;
	HeapTuple tuple;
	Datum	  values[4] = {0};
	bool	  nulls[4]	= {false, true, true, true};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bm25_index_info requires a composite result type")));
	tupdesc = BlessTupleDesc(tupdesc);

	if (!info->header_present)
	{
		nulls[0] = true;
		return HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls));
	}

	values[0] = Int64GetDatum((int64)info->version);
	if (info->capabilities_present)
	{
		values[1] = Int64GetDatum((int64)info->capabilities);
		values[2] = BoolGetDatum(
				(info->capabilities & TP_METAPAGE_ALL_DOCUMENTS_INDEXED) != 0);
		nulls[1] = nulls[2] = false;
	}
	if (info->legacy_docid_pointer_known)
	{
		values[3] = BoolGetDatum(info->legacy_docid_pointer_present);
		nulls[3]  = false;
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	return HeapTupleGetDatum(tuple);
}

PG_FUNCTION_INFO_V1(tp_index_info);

Datum
tp_index_info(PG_FUNCTION_ARGS)
{
	Oid				  index_oid = PG_GETARG_OID(0);
	Relation		  index_rel;
	TpIndexHeaderInfo info;

	index_rel = tp_open_bm25_index(
			index_oid, AccessShareLock, TP_INDEX_ACCESS_MAINTAIN);
	info = tp_inspect_metapage(index_rel);
	relation_close(index_rel, AccessShareLock);

	PG_RETURN_DATUM(tp_index_header_info_datum(fcinfo, &info));
}
