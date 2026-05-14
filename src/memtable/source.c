/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * source.c - Memtable implementation of TpDataSource
 */
#include <postgres.h>

#include <lib/dshash.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "index/source.h"
#include "memtable/chain_source.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/source.h"
#include "memtable/stringtable.h"

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

	if (!posting_list)
		return NULL;

	LWLockAcquire(&posting_list->lock, LW_SHARED);

	if (posting_list->doc_count == 0)
	{
		LWLockRelease(&posting_list->lock);
		return NULL;
	}

	entries = tp_get_posting_entries(ms->local_state->dsa, posting_list);
	if (!entries)
	{
		LWLockRelease(&posting_list->lock);
		return NULL;
	}

	/* Copy to local memory under the lock */
	data		   = tp_alloc_posting_data(posting_list->doc_count);
	data->count	   = posting_list->doc_count;
	data->doc_freq = posting_list->doc_freq > 0 ? posting_list->doc_freq
												: posting_list->doc_count;

	for (i = 0; i < posting_list->doc_count; i++)
	{
		data->ctids[i]		 = entries[i].ctid;
		data->frequencies[i] = entries[i].frequency;
	}

	LWLockRelease(&posting_list->lock);

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
 *
 * Phase 4 of issue #374: this is now a thin wrapper around
 * tp_memtable_chain_source_create.  The legacy DSA-backed
 * implementation (memtable_get_postings / memtable_get_doc_length /
 * memtable_close above) is no longer reachable from production
 * code; the dshash machinery and TpMemtableSource struct remain
 * compiled but unused until Phase 7 teardown.
 */
TpDataSource *
tp_memtable_source_create(TpLocalIndexState *local_state, Relation rel)
{
	Assert(local_state != NULL);

	return tp_memtable_chain_source_create(local_state, rel);
}
