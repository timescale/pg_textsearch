/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * vacuum.c - BM25 index vacuum and maintenance operations
 */
#include <postgres.h>

#include <access/genam.h>
#include <catalog/namespace.h>
#include <commands/progress.h>
#include <commands/vacuum.h>
#include <fmgr.h>
#include <lib/dshash.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/regproc.h>
#include <utils/rel.h>

#include "am.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "segment/merge.h"
#include "segment/segment.h"
#include "state/metapage.h"
#include "state/state.h"

/*
 * Iterate through memtable CTIDs and invoke callback for each
 * Returns number of tuples reported
 */
static int64
tp_bulkdelete_memtable_ctids(
		TpLocalIndexState	   *index_state,
		IndexBulkDeleteCallback callback,
		void				   *callback_state)
{
	TpMemtable		 *memtable;
	dshash_table	 *doc_lengths_table;
	dshash_seq_status seq_status;
	TpDocLengthEntry *entry;
	int64			  count = 0;

	if (!index_state || !index_state->shared)
		return 0;

	memtable = get_memtable(index_state);
	if (!memtable || memtable->doc_lengths_handle == DSHASH_HANDLE_INVALID)
		return 0;

	/* Attach to the document lengths hash table */
	doc_lengths_table = tp_doclength_table_attach(
			index_state->dsa, memtable->doc_lengths_handle);
	if (!doc_lengths_table)
		return 0;

	/* Iterate through all entries and call the callback for each CTID */
	dshash_seq_init(&seq_status, doc_lengths_table, false);
	while ((entry = dshash_seq_next(&seq_status)) != NULL)
	{
		if (ItemPointerIsValid(&entry->ctid))
		{
			/* Call the callback - returns true if tuple should be deleted */
			(void)callback(&entry->ctid, callback_state);
			count++;
		}
	}
	dshash_seq_term(&seq_status);
	dshash_detach(doc_lengths_table);

	return count;
}

/*
 * Iterate through segment CTIDs and invoke callback for each
 * Returns number of tuples reported
 */
static int64
tp_bulkdelete_segment_ctids(
		Relation				index,
		BlockNumber				first_segment,
		IndexBulkDeleteCallback callback,
		void				   *callback_state)
{
	BlockNumber segment_root = first_segment;
	int64		count		 = 0;

	while (segment_root != InvalidBlockNumber)
	{
		TpSegmentReader *reader;
		uint32			 num_docs;
		uint32			 i;

		reader = tp_segment_open_ex(index, segment_root, true);
		if (!reader || !reader->header)
		{
			if (reader)
				tp_segment_close(reader);
			break;
		}

		num_docs = reader->header->num_docs;

		/* Iterate through all documents in this segment */
		for (i = 0; i < num_docs; i++)
		{
			ItemPointerData ctid;

			tp_segment_lookup_ctid(reader, i, &ctid);
			if (ItemPointerIsValid(&ctid))
			{
				(void)callback(&ctid, callback_state);
				count++;
			}
		}

		segment_root = reader->header->next_segment;
		tp_segment_close(reader);
	}

	return count;
}

/*
 * Bulk delete callback for vacuum and CREATE INDEX CONCURRENTLY
 *
 * This function serves two purposes:
 * 1. During VACUUM: Called with a callback that returns true for dead tuples
 * 2. During CREATE INDEX CONCURRENTLY validation: Called with a callback
 *    that collects TIDs to determine which tuples are already indexed
 *
 * The callback receives each indexed CTID and returns true if the tuple
 * should be deleted (for VACUUM) or false (for CIC validation which just
 * collects TIDs).
 */
IndexBulkDeleteResult *
tp_bulkdelete(
		IndexVacuumInfo		   *info,
		IndexBulkDeleteResult  *stats,
		IndexBulkDeleteCallback callback,
		void				   *callback_state)
{
	TpIndexMetaPage	   metap;
	TpLocalIndexState *index_state;
	int64			   memtable_count = 0;
	int64			   segment_count  = 0;

	/* Initialize stats structure if not provided */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	/* Get current index statistics from metapage */
	metap = tp_get_metapage(info->index);
	if (!metap)
	{
		/* Couldn't read metapage, return minimal stats */
		stats->num_pages		= 0;
		stats->num_index_tuples = 0;
		stats->tuples_removed	= 0;
		stats->pages_deleted	= 0;

		elog(WARNING,
			 "Tapir bulkdelete: couldn't read metapage for index %s",
			 RelationGetRelationName(info->index));
		return stats;
	}

	/*
	 * If callback is provided, iterate through all indexed CTIDs and
	 * invoke it. This is required for CREATE INDEX CONCURRENTLY validation,
	 * which uses this to collect existing TIDs before the second scan.
	 */
	if (callback != NULL)
	{
		/* Get index state for memtable access */
		index_state = tp_get_local_index_state(RelationGetRelid(info->index));

		/* Iterate memtable CTIDs */
		if (index_state != NULL)
		{
			memtable_count = tp_bulkdelete_memtable_ctids(
					index_state, callback, callback_state);
		}

		/* Iterate segment CTIDs at all levels */
		for (int level = 0; level < TP_MAX_LEVELS; level++)
		{
			if (metap->level_heads[level] != InvalidBlockNumber)
			{
				segment_count += tp_bulkdelete_segment_ctids(
						info->index,
						metap->level_heads[level],
						callback,
						callback_state);
			}
		}
	}

	/* Suppress unused variable warnings - counts used for debugging */
	(void)memtable_count;
	(void)segment_count;

	/* Fill in statistics */
	stats->num_pages		= 1; /* Minimal pages (just metapage) */
	stats->num_index_tuples = (double)metap->total_docs;
	stats->tuples_removed	= 0;
	stats->pages_deleted	= 0;

	pfree(metap);

	return stats;
}

/*
 * Vacuum/cleanup the BM25 index
 */
IndexBulkDeleteResult *
tp_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	TpIndexMetaPage metap;

	/* Initialize stats if not provided */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	/* Get current index statistics from metapage */
	metap = tp_get_metapage(info->index);
	if (metap)
	{
		/* Update statistics with current values */
		stats->num_pages		= 1; /* Minimal pages (just metapage) */
		stats->num_index_tuples = (double)metap->total_docs;

		/* Report current usage statistics */
		if (stats->pages_deleted == 0 && stats->tuples_removed == 0)
		{
			/* No deletions recorded, report full statistics */
			stats->pages_free = 0; /* No free pages in memtable impl */
		}

		pfree(metap);
	}
	else
	{
		elog(WARNING,
			 "Tapir vacuum cleanup: couldn't read metapage for index %s",
			 RelationGetRelationName(info->index));

		/* Keep existing stats if available, otherwise initialize */
		if (stats->num_pages == 0 && stats->num_index_tuples == 0)
		{
			stats->num_pages		= 1; /* At least the metapage */
			stats->num_index_tuples = 0;
		}
	}

	return stats;
}
