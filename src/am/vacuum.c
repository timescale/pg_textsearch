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

/* Tapir-specific build phases */
#define TP_PHASE_LOADING 2
#define TP_PHASE_WRITING 3

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

/*
 * Build phase name for progress reporting
 */
char *
tp_buildphasename(int64 phase)
{
	switch (phase)
	{
	case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
		return "initializing";
	case TP_PHASE_LOADING:
		return "loading tuples";
	case TP_PHASE_WRITING:
		return "writing index";
	default:
		return NULL;
	}
}

/*
 * tp_spill_memtable - Force memtable flush to disk segment
 *
 * This function allows manual triggering of segment writes.
 * Returns the block number of the written segment, or NULL if memtable was
 * empty.
 */
PG_FUNCTION_INFO_V1(tp_spill_memtable);

Datum
tp_spill_memtable(PG_FUNCTION_ARGS)
{
	text			  *index_name_text = PG_GETARG_TEXT_PP(0);
	char			  *index_name	   = text_to_cstring(index_name_text);
	Oid				   index_oid;
	Relation		   index_rel;
	TpLocalIndexState *index_state;
	BlockNumber		   segment_root;
	RangeVar		  *rv;

	/* Parse index name (supports schema.index notation) */
	rv = makeRangeVarFromNameList(stringToQualifiedNameList(index_name, NULL));
	index_oid = RangeVarGetRelid(rv, AccessShareLock, false);

	if (!OidIsValid(index_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" does not exist", index_name)));

	/* Open the index */
	index_rel = index_open(index_oid, RowExclusiveLock);

	/* Get index state */
	index_state = tp_get_local_index_state(RelationGetRelid(index_rel));
	if (!index_state)
	{
		index_close(index_rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for \"%s\"", index_name)));
	}

	/* Acquire exclusive lock for write operation */
	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

	/* Write the segment */
	segment_root = tp_write_segment(index_state, index_rel);

	/* Clear the memtable after successful spilling */
	if (segment_root != InvalidBlockNumber)
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;

		tp_clear_memtable(index_state);

		/* Link new segment as L0 chain head */
		metabuf = ReadBuffer(index_rel, 0);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		if (metap->level_heads[0] != InvalidBlockNumber)
		{
			/* Point new segment to old chain head */
			Buffer			 seg_buf;
			Page			 seg_page;
			TpSegmentHeader *seg_header;

			seg_buf = ReadBuffer(index_rel, segment_root);
			LockBuffer(seg_buf, BUFFER_LOCK_EXCLUSIVE);
			seg_page				 = BufferGetPage(seg_buf);
			seg_header				 = (TpSegmentHeader *)((char *)seg_page +
											   SizeOfPageHeaderData);
			seg_header->next_segment = metap->level_heads[0];
			MarkBufferDirty(seg_buf);
			UnlockReleaseBuffer(seg_buf);
		}

		metap->level_heads[0] = segment_root;
		metap->level_counts[0]++;
		MarkBufferDirty(metabuf);

		UnlockReleaseBuffer(metabuf);

		/* Check if L0 needs compaction */
		tp_maybe_compact_level(index_rel, 0);
	}

	/* Release lock */
	tp_release_index_lock(index_state);

	/* Close the index */
	index_close(index_rel, RowExclusiveLock);

	/* Return block number or NULL */
	if (segment_root != InvalidBlockNumber)
	{
		PG_RETURN_INT32(segment_root);
	}
	else
	{
		PG_RETURN_NULL();
	}
}
