/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * source.c - Common helpers for data source interface
 */
#include <postgres.h>

#include <utils/memutils.h>

#include "source.h"

/*
 * Allocate posting data with given capacity.
 * Arrays are allocated but not initialized.
 */
TpPostingData *
tp_alloc_posting_data(int32 capacity)
{
	TpPostingData *data;

	data		= (TpPostingData *)palloc(sizeof(TpPostingData));
	data->ctids = (ItemPointerData *)palloc(
			capacity * sizeof(ItemPointerData));
	data->frequencies = (int32 *)palloc(capacity * sizeof(int32));
	data->count		  = 0;
	data->doc_freq	  = 0;

	return data;
}

/*
 * Free posting data allocated by tp_alloc_posting_data().
 */
void
tp_free_posting_data(TpPostingData *data)
{
	if (data)
	{
		if (data->ctids)
			pfree(data->ctids);
		if (data->frequencies)
			pfree(data->frequencies);
		pfree(data);
	}
}
