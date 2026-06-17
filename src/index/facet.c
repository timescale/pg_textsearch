/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * facet.c - Faceted-search filter pushdown
 *
 * See facet.h for an overview. The interesting part is that the allow-list is
 * consulted inside BMW scoring (next to the dead-doc skip) so that the top-k
 * threshold reflects only documents matching the facet, rather than relying on
 * an over-fetch + post-filter.
 */
#include <postgres.h>

#include <access/heapam.h>
#include <access/htup_details.h>
#include <access/relscan.h>
#include <access/table.h>
#include <access/tableam.h>
#include <executor/tuptable.h>
#include <fmgr.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <utils/datum.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>

#include "index/facet.h"

/* GUCs (registered in mod.c). */
bool   tp_enable_facet_pushdown		  = true;
double tp_facet_selectivity_threshold = 0.12;

/*
 * Per-backend facet spec captured during planning. Mirrors index/limit.c.
 * The value Datum (if by-reference) lives in TopMemoryContext.
 */
typedef struct TpFacetSpec
{
	bool	   is_valid;
	Oid		   bm25_index_oid;
	Oid		   heap_oid;
	AttrNumber attno;
	Oid		   opno;
	Oid		   collation;
	bool	   var_on_left;
	Datum	   value;
	int16	   typlen;
	bool	   typbyval;
} TpFacetSpec;

static TpFacetSpec tp_pending_facet = {0};

/* Active allow-list consulted by BMW during a scoring run. */
static TpFacetFilter *tp_active_facet = NULL;

/* Drop any by-reference value held by the pending spec. */
static void
tp_facet_free_pending_value(void)
{
	if (tp_pending_facet.is_valid && !tp_pending_facet.typbyval &&
		DatumGetPointer(tp_pending_facet.value) != NULL)
	{
		pfree(DatumGetPointer(tp_pending_facet.value));
		tp_pending_facet.value = (Datum)0;
	}
}

void
tp_store_query_facet(
		Oid		   bm25_index_oid,
		Oid		   heap_oid,
		AttrNumber attno,
		Oid		   opno,
		Oid		   collation,
		bool	   var_on_left,
		Datum	   value,
		int16	   typlen,
		bool	   typbyval)
{
	MemoryContext oldcontext;

	if (!tp_enable_facet_pushdown)
		return;

	/* Replace any previous spec. */
	tp_facet_free_pending_value();

	tp_pending_facet.bm25_index_oid = bm25_index_oid;
	tp_pending_facet.heap_oid		= heap_oid;
	tp_pending_facet.attno			= attno;
	tp_pending_facet.opno			= opno;
	tp_pending_facet.collation		= collation;
	tp_pending_facet.var_on_left	= var_on_left;
	tp_pending_facet.typlen			= typlen;
	tp_pending_facet.typbyval		= typbyval;

	/* Copy the constant into a persistent context (planning context is short).
	 */
	oldcontext			   = MemoryContextSwitchTo(TopMemoryContext);
	tp_pending_facet.value = datumCopy(value, typbyval, typlen);
	MemoryContextSwitchTo(oldcontext);

	tp_pending_facet.is_valid = true;
}

/* bsearch/qsort comparator over heap TIDs. */
static int
tp_tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer)a, (ItemPointer)b);
}

/*
 * Remap each collected heap TID from the live tuple it points at to the
 * root of its HOT update chain.
 *
 * This is essential for correctness. A heap (sequential) scan yields the
 * *live* tuple of each row, whose CTID is the tip of any HOT update chain.
 * The BM25 index, like every Postgres index, stores the CTID of the HOT
 * chain *root* (that is what the index build callback receives). After even
 * a single HOT update the two diverge, so an allow-list built from live
 * CTIDs would fail to match the segment's stored CTIDs and silently exclude
 * matching documents from the top-k.
 *
 * heap_get_root_tuples() maps every offset on a page to its chain root, and
 * because HOT chains never span pages the remap stays within the same block.
 * For un-updated rows the root is the tuple itself, so this is a no-op.
 */
static void
tp_remap_tids_to_hot_roots(Relation heap, ItemPointerData *tids, int count)
{
	BlockNumber	 cur_blk = InvalidBlockNumber;
	Buffer		 buf	 = InvalidBuffer;
	OffsetNumber root_offsets[MaxHeapTuplesPerPage];
	int			 i;

	for (i = 0; i < count; i++)
	{
		BlockNumber	 blk = ItemPointerGetBlockNumber(&tids[i]);
		OffsetNumber off = ItemPointerGetOffsetNumber(&tids[i]);
		OffsetNumber root;

		if (blk != cur_blk)
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			buf = ReadBuffer(heap, blk);
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			heap_get_root_tuples(BufferGetPage(buf), root_offsets);
			cur_blk = blk;
		}

		root = root_offsets[off - 1];
		if (OffsetNumberIsValid(root))
			ItemPointerSetOffsetNumber(&tids[i], root);
	}

	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

TpFacetFilter *
tp_build_query_facet(Relation bm25_index, MemoryContext ctx)
{
	TpFacetFilter	*filter;
	Relation		 heap;
	TableScanDesc	 scan;
	TupleTableSlot	*slot;
	Snapshot		 snapshot;
	FmgrInfo		 opproc;
	MemoryContext	 oldcontext;
	ItemPointerData *tids;
	int				 capacity;
	int				 count = 0;

	if (!tp_enable_facet_pushdown || !tp_pending_facet.is_valid)
		return NULL;

	if (!RelationIsValid(bm25_index) ||
		RelationGetRelid(bm25_index) != tp_pending_facet.bm25_index_oid)
		return NULL;

	/* One-shot: consume the spec regardless of outcome below. */
	tp_pending_facet.is_valid = false;

	fmgr_info(get_opcode(tp_pending_facet.opno), &opproc);

	heap	 = table_open(tp_pending_facet.heap_oid, AccessShareLock);
	snapshot = GetActiveSnapshot();
	slot	 = table_slot_create(heap, NULL);
	scan	 = table_beginscan(heap, snapshot, 0, NULL);

	oldcontext = MemoryContextSwitchTo(ctx);
	capacity   = 1024;
	tids	   = palloc(capacity * sizeof(ItemPointerData));
	MemoryContextSwitchTo(oldcontext);

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		Datum attval;
		Datum result;
		bool  isnull;

		attval = slot_getattr(slot, tp_pending_facet.attno, &isnull);
		if (isnull)
			continue;

		if (tp_pending_facet.var_on_left)
			result = FunctionCall2Coll(
					&opproc,
					tp_pending_facet.collation,
					attval,
					tp_pending_facet.value);
		else
			result = FunctionCall2Coll(
					&opproc,
					tp_pending_facet.collation,
					tp_pending_facet.value,
					attval);

		if (!DatumGetBool(result))
			continue;

		if (count >= capacity)
		{
			oldcontext = MemoryContextSwitchTo(ctx);
			capacity *= 2;
			tids = repalloc(tids, capacity * sizeof(ItemPointerData));
			MemoryContextSwitchTo(oldcontext);
		}
		tids[count++] = slot->tts_tid;
	}

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);

	/*
	 * Translate live-tuple CTIDs to HOT-chain roots so the allow-list matches
	 * the CTIDs the BM25 segment stored at build time. Done while the heap is
	 * still open because it reads heap buffers.
	 */
	if (count > 0)
		tp_remap_tids_to_hot_roots(heap, tids, count);

	table_close(heap, AccessShareLock);

	elog(DEBUG1,
		 "facet pushdown: collected %d matching TID(s) for index \"%s\"",
		 count,
		 RelationGetRelationName(bm25_index));

	if (count == 0)
	{
		/* Empty allow-list still suppresses all results (correct). */
		pfree(tids);
		tids = NULL;
	}
	else
	{
		qsort(tids, count, sizeof(ItemPointerData), tp_tid_cmp);
	}

	oldcontext = MemoryContextSwitchTo(ctx);
	filter	   = palloc(sizeof(TpFacetFilter));
	MemoryContextSwitchTo(oldcontext);
	filter->tids  = tids;
	filter->count = count;

	return filter;
}

void
tp_cleanup_query_facets(void)
{
	tp_facet_free_pending_value();
	tp_pending_facet.is_valid = false;
	tp_active_facet			  = NULL;
}

void
tp_facet_set_active(TpFacetFilter *filter)
{
	tp_active_facet = filter;
}

void
tp_facet_clear_active(void)
{
	tp_active_facet = NULL;
}

bool
tp_facet_active(void)
{
	return tp_active_facet != NULL;
}

bool
tp_facet_excludes(ItemPointer ctid)
{
	if (tp_active_facet == NULL)
		return false;

	/* An active filter with no TIDs excludes everything. */
	if (tp_active_facet->count == 0)
		return true;

	return bsearch(ctid,
				   tp_active_facet->tids,
				   tp_active_facet->count,
				   sizeof(ItemPointerData),
				   tp_tid_cmp) == NULL;
}
