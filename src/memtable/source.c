/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * memtable/source.c - Memtable implementation of TpDataSource
 */
#include <postgres.h>

#include <lib/dshash.h>
#include <utils/memutils.h>

#include "../source.h"
#include "memtable.h"
#include "posting.h"
#include "source.h"
#include "stringtable.h"

/*
 * Memtable-specific data source state
 */
typedef struct TpMemtableSource
{
	TpDataSource	   base;			/* Must be first */
	TpLocalIndexState *local_state;		/* Index state */
	dshash_table	  *string_table;	/* Attached string table */
	dshash_table	  *doclength_table; /* Attached doc length table */
} TpMemtableSource;

/*
 * Get posting data for a term from memtable.
 * Returns columnar data with parallel ctid and frequency arrays.
 */
static TpPostingData *
memtable_get_postings(TpDataSource *source, const char *term)
{
	TpMemtableSource *ms = (TpMemtableSource *)source;
	TpPostingList	 *posting_list;
	TpPostingEntry	 *entries;
	TpPostingData	 *data;
	int				  i;

	if (!ms->string_table)
		return NULL;

	posting_list = tp_string_table_get_posting_list(
			ms->local_state->dsa, ms->string_table, term);

	if (!posting_list || posting_list->doc_count == 0)
		return NULL;

	entries = tp_get_posting_entries(ms->local_state->dsa, posting_list);
	if (!entries)
		return NULL;

	/* Convert to columnar format */
	data		   = tp_alloc_posting_data(posting_list->doc_count);
	data->count	   = posting_list->doc_count;
	data->doc_freq = posting_list->doc_freq > 0 ? posting_list->doc_freq
												: posting_list->doc_count;

	for (i = 0; i < posting_list->doc_count; i++)
	{
		data->ctids[i]		 = entries[i].ctid;
		data->frequencies[i] = entries[i].frequency;
	}

	return data;
}

/*
 * Free posting data - just use the common helper.
 */
static void
memtable_free_postings(TpDataSource *source, TpPostingData *data)
{
	(void)source; /* unused */
	tp_free_posting_data(data);
}

/*
 * Get document length for a CTID from memtable.
 */
static int32
memtable_get_doc_length(TpDataSource *source, ItemPointer ctid)
{
	TpMemtableSource *ms = (TpMemtableSource *)source;

	if (!ms->doclength_table)
		return -1;

	return tp_get_document_length_attached(ms->doclength_table, ctid);
}

/*
 * Close the memtable source and free resources.
 */
static void
memtable_close(TpDataSource *source)
{
	TpMemtableSource *ms = (TpMemtableSource *)source;

	if (ms->string_table)
		dshash_detach(ms->string_table);
	if (ms->doclength_table)
		dshash_detach(ms->doclength_table);

	pfree(ms);
}

/* Virtual function table for memtable source */
static const TpDataSourceOps memtable_source_ops = {
		.get_postings	= memtable_get_postings,
		.free_postings	= memtable_free_postings,
		.get_doc_length = memtable_get_doc_length,
		.close			= memtable_close,
};

/*
 * Create a data source that reads from the memtable.
 */
TpDataSource *
tp_memtable_source_create(TpLocalIndexState *local_state)
{
	TpMemtableSource *ms;
	TpMemtable		 *memtable;

	Assert(local_state != NULL);

	memtable = get_memtable(local_state);
	if (!memtable)
		return NULL;

	ms			 = (TpMemtableSource *)palloc0(sizeof(TpMemtableSource));
	ms->base.ops = &memtable_source_ops;
	ms->base.total_docs = local_state->shared->total_docs;
	ms->base.total_len	= local_state->shared->total_len;
	ms->local_state		= local_state;

	/* Attach to string table if available */
	if (memtable->string_hash_handle != DSHASH_HANDLE_INVALID)
	{
		ms->string_table = tp_string_table_attach(
				local_state->dsa, memtable->string_hash_handle);
	}

	/* Attach to doc length table if available */
	if (memtable->doc_lengths_handle != DSHASH_HANDLE_INVALID)
	{
		ms->doclength_table = tp_doclength_table_attach(
				local_state->dsa, memtable->doc_lengths_handle);
	}

	return (TpDataSource *)ms;
}
