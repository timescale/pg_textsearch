/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am/vacuum.c - BM25 index vacuum and maintenance operations
 */
#include <postgres.h>

#include <access/genam.h>
#include <catalog/namespace.h>
#include <commands/progress.h>
#include <commands/vacuum.h>
#include <fmgr.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/regproc.h>
#include <utils/rel.h>

#include "am.h"
#include "memtable/memtable.h"
#include "segment/merge.h"
#include "segment/segment.h"
#include "state/metapage.h"
#include "state/state.h"

/*
 * Bulk delete callback for vacuum
 */
IndexBulkDeleteResult *
tp_bulkdelete(
		IndexVacuumInfo		   *info,
		IndexBulkDeleteResult  *stats,
		IndexBulkDeleteCallback callback __attribute__((unused)),
		void				   *callback_state __attribute__((unused)))
{
	TpIndexMetaPage metap;

	/* Initialize stats structure if not provided */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	/* Get current index statistics from metapage */
	metap = tp_get_metapage(info->index);
	if (metap)
	{
		stats->num_pages		= 1; /* Minimal pages (just metapage) */
		stats->num_index_tuples = (double)metap->total_docs;

		/* Track that deletion was requested */
		stats->tuples_removed = 0;
		stats->pages_deleted  = 0;

		pfree(metap);
	}
	else
	{
		/* Couldn't read metapage, return minimal stats */
		stats->num_pages		= 0;
		stats->num_index_tuples = 0;
		stats->tuples_removed	= 0;
		stats->pages_deleted	= 0;

		elog(WARNING,
			 "Tapir bulkdelete: couldn't read metapage for index %s",
			 RelationGetRelationName(info->index));
	}

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
