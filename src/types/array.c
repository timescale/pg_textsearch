/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * array.c - Text array helpers for BM25 indexing
 *
 * Provides utilities for flattening text-like array columns
 * (text[], varchar[], bpchar[]) into a single text value for
 * tokenization by the BM25 index build, insert, and scoring
 * paths.
 */
#include <postgres.h>

#include <catalog/pg_type.h>
#include <utils/array.h>
#include <utils/builtins.h>

#include "array.h"

/*
 * Is this type OID a text-like array (text[], varchar[], bpchar[])?
 */
bool
tp_is_text_array_type(Oid typid)
{
	return typid == TEXTARRAYOID || typid == 1015 || /* varchar[] */
		   typid == 1014;							 /* bpchar[] */
}

/*
 * Flatten a text array into a single space-separated text value.
 * NULL elements are skipped. Returns empty text for empty arrays.
 * Works for text[], varchar[], and bpchar[] (same binary format).
 */
text *
tp_flatten_text_array(Datum array_datum)
{
	ArrayType	  *arr;
	Datum		  *elems;
	bool		  *nulls;
	int			   nelems;
	StringInfoData buf;

	arr = DatumGetArrayTypeP(array_datum);
	deconstruct_array(
			arr, TEXTOID, -1, false, TYPALIGN_INT, &elems, &nulls, &nelems);

	initStringInfo(&buf);

	for (int i = 0; i < nelems; i++)
	{
		text *elem;
		int	  len;

		if (nulls[i])
			continue;

		elem = DatumGetTextPP(elems[i]);
		len	 = VARSIZE_ANY_EXHDR(elem);

		if (len == 0)
			continue;

		if (buf.len > 0)
			appendStringInfoChar(&buf, ' ');
		appendBinaryStringInfo(&buf, VARDATA_ANY(elem), len);
	}

	return cstring_to_text_with_len(buf.data, buf.len);
}
