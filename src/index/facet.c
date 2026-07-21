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

#include <access/amapi.h>
#include <access/genam.h>
#include <access/heapam.h>
#include <access/htup_details.h>
#include <access/relscan.h>
#include <access/skey.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/transam.h>
#include <executor/tuptable.h>
#include <fmgr.h>
#include <nodes/nodeFuncs.h>
#include <nodes/primnodes.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <utils/datum.h>
#include <utils/fmgroids.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/relcache.h>
#include <utils/snapmgr.h>

#include "index/facet.h"

/* GUCs (registered in mod.c). */
bool   tp_enable_facet_pushdown		  = true;
double tp_facet_selectivity_threshold = 0.02;
bool   tp_log_facet					  = false;

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
	Oid		   consttype;
	int16	   typlen;
	bool	   typbyval;
} TpFacetSpec;

static TpFacetSpec tp_pending_facet = {0};

/* Active allow-list consulted by BMW during a scoring run. */
static TpFacetFilter *tp_active_facet = NULL;

/*
 * Drop any by-reference value held by the pending spec. Not gated on
 * is_valid: after the spec is consumed (is_valid cleared) the copied value
 * still needs reclaiming, so freeing keys on the pointer itself.
 */
static void
tp_facet_free_pending_value(void)
{
	if (!tp_pending_facet.typbyval &&
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
		Oid		   consttype,
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
	tp_pending_facet.consttype		= consttype;
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

/*
 * Build the allow-list via an index scan on the facet column instead of a full
 * heap scan, turning allow-list construction from O(table) into O(matching
 * rows) -- the change that makes the pushdown pay off at scale.
 *
 * Scans the first suitable index on the heap: valid, non-partial, an AM with
 * amgettuple, whose leading key column is the facet column and whose opclass
 * supports the facet operator with a matching collation. Index entries
 * reference HOT-chain roots (as the BM25 segment does), so unlike the heap
 * scan this path needs no HOT-root remap. index_getnext_tid yields every
 * matching index entry without a heap visibility check -- a safe superset,
 * since the Filter node above the scan is the exact recheck.
 *
 * Returns a palloc'd (in ctx) array of matching heap TIDs and sets *out_count
 * to the number found (>= 0). Returns NULL with *out_count == -1 when no
 * suitable index exists, so the caller falls back to the heap scan.
 */
static ItemPointerData *
tp_build_facet_via_index(
		Relation	  heap,
		Snapshot	  snapshot,
		MemoryContext ctx,
		int			 *out_count,
		char		 *idxname,
		size_t		  idxname_len)
{
	List			*indexoids = RelationGetIndexList(heap);
	ListCell		*lc;
	ItemPointerData *tids  = NULL;
	int				 count = -1;

	foreach (lc, indexoids)
	{
		Oid			  indexoid = lfirst_oid(lc);
		Relation	  irel	   = index_open(indexoid, AccessShareLock);
		Oid			  opfamily = irel->rd_opfamily[0];
		Oid			  scan_opno;
		int			  strategy;
		Oid			  lefttype;
		Oid			  righttype;
		ScanKeyData	  skey;
		IndexScanDesc iscan;
		ItemPointer	  tid;
		int			  capacity;
		MemoryContext oldcontext;
#if PG_VERSION_NUM >= 180000
		IndexScanInstrumentation instr;
#endif

		/* Commute "const OP var" into "var OP const" for the index scan. */
		scan_opno = tp_pending_facet.var_on_left
						  ? tp_pending_facet.opno
						  : get_commutator(tp_pending_facet.opno);

		/*
		 * Reject indexes that can't safely/efficiently answer the facet:
		 * invalid, still-suspect broken HOT chains under our snapshot
		 * (indcheckxmin, mirroring the planner in plancat.c), partial (would
		 * miss rows -> false negatives), no amgettuple, leading key not the
		 * facet column, operator not in the opclass, or a collation mismatch.
		 */
		if (!irel->rd_index->indisvalid ||
			(irel->rd_index->indcheckxmin &&
			 !TransactionIdPrecedes(
					 HeapTupleHeaderGetXmin(irel->rd_indextuple->t_data),
					 TransactionXmin)) ||
			RelationGetIndexPredicate(irel) != NIL ||
			irel->rd_indam->amgettuple == NULL ||
			irel->rd_index->indkey.values[0] != tp_pending_facet.attno ||
			!OidIsValid(scan_opno) || !op_in_opfamily(scan_opno, opfamily) ||
			irel->rd_indcollation[0] != tp_pending_facet.collation)
		{
			index_close(irel, AccessShareLock);
			continue;
		}

		get_op_opfamily_properties(
				scan_opno, opfamily, false, &strategy, &lefttype, &righttype);

		oldcontext = MemoryContextSwitchTo(ctx);
		capacity   = 1024;
		tids	   = palloc(capacity * sizeof(ItemPointerData));
		MemoryContextSwitchTo(oldcontext);
		count = 0;

		/*
		 * Pass the operator's right-hand type as the scan-key subtype (as
		 * ExecIndexBuildScanKeys does) so a cross-type facet operator uses the
		 * correct cross-type comparison/hash support proc. InvalidOid would
		 * mean "same as the index column type", which misreads a differently
		 * typed argument -- harmless for btree (recheck via the operator) but
		 * silently wrong for hash (wrong bucket -> empty allow-list). For a
		 * same-type operator righttype == the column type, so this is a no-op.
		 */
		ScanKeyEntryInitialize(
				&skey,
				0,
				1, /* leading index column */
				(StrategyNumber)strategy,
				righttype,
				tp_pending_facet.collation,
				get_opcode(scan_opno),
				tp_pending_facet.value);

#if PG_VERSION_NUM >= 180000
		memset(&instr, 0, sizeof(instr));
		iscan = index_beginscan(heap, irel, snapshot, &instr, 1, 0);
#else
		iscan = index_beginscan(heap, irel, snapshot, 1, 0);
#endif
		index_rescan(iscan, &skey, 1, NULL, 0);

		while ((tid = index_getnext_tid(iscan, ForwardScanDirection)) != NULL)
		{
			if (count >= capacity)
			{
				oldcontext = MemoryContextSwitchTo(ctx);
				capacity *= 2;
				tids = repalloc(tids, capacity * sizeof(ItemPointerData));
				MemoryContextSwitchTo(oldcontext);
			}
			tids[count++] = *tid;
		}

		index_endscan(iscan);
		strlcpy(idxname, RelationGetRelationName(irel), idxname_len);
		index_close(irel, AccessShareLock);
		break; /* first suitable index wins */
	}

	list_free(indexoids);
	*out_count = count;
	return tids;
}

/*
 * Fallback: build the allow-list with a full heap scan, evaluating the facet
 * operator on every live tuple. Used when no suitable facet index exists. Live
 * tuples are the tip of any HOT chain, so their CTIDs are remapped to chain
 * roots to match the CTIDs the BM25 segment stored at build time.
 */
static ItemPointerData *
tp_build_facet_via_heap(
		Relation heap, Snapshot snapshot, MemoryContext ctx, int *out_count)
{
	FmgrInfo		 opproc;
	TableScanDesc	 scan;
	TupleTableSlot	*slot;
	MemoryContext	 oldcontext;
	ItemPointerData *tids;
	int				 capacity;
	int				 count = 0;

	fmgr_info(get_opcode(tp_pending_facet.opno), &opproc);
	slot = table_slot_create(heap, NULL);
	scan = table_beginscan(heap, snapshot, 0, NULL);

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

	if (count > 0)
		tp_remap_tids_to_hot_roots(heap, tids, count);

	*out_count = count;
	return tids;
}

TpFacetFilter *
tp_build_query_facet(Relation bm25_index, Snapshot snapshot, MemoryContext ctx)
{
	TpFacetFilter	*filter;
	Relation		 heap;
	MemoryContext	 oldcontext;
	ItemPointerData *tids;
	int				 count;
	bool			 used_index;
	char			 idxname[NAMEDATALEN];

	if (!tp_enable_facet_pushdown || !tp_pending_facet.is_valid)
		return NULL;

	if (!RelationIsValid(bm25_index) ||
		RelationGetRelid(bm25_index) != tp_pending_facet.bm25_index_oid)
		return NULL;

	/* One-shot: consume the spec regardless of outcome below. */
	tp_pending_facet.is_valid = false;

	heap = table_open(tp_pending_facet.heap_oid, AccessShareLock);

	/*
	 * Build the allow-list under the scan's own snapshot so its visibility
	 * matches the BM25 scan (the heap fallback applies MVCC visibility; using
	 * a different snapshot could drop a row the scan can see).
	 */
	if (snapshot == NULL)
		snapshot = GetActiveSnapshot();

	/*
	 * Prefer an index scan on the facet column (O(matching rows)); fall back
	 * to a full heap scan (O(table)) only when no suitable index exists.
	 */
	tids = tp_build_facet_via_index(
			heap, snapshot, ctx, &count, idxname, sizeof(idxname));
	used_index = (count >= 0);
	if (!used_index)
		tids = tp_build_facet_via_heap(heap, snapshot, ctx, &count);

	table_close(heap, AccessShareLock);

	if (tp_log_facet)
	{
		if (used_index)
			elog(NOTICE,
				 "facet pushdown: index scan via \"%s\" (%d tids)",
				 idxname,
				 count);
		else
			elog(NOTICE, "facet pushdown: heap scan (%d tids)", count);
	}

	elog(DEBUG1,
		 "facet pushdown: collected %d matching TID(s) for index \"%s\"",
		 count,
		 RelationGetRelationName(bm25_index));

	if (count == 0)
	{
		/* Empty allow-list still suppresses all results (correct). */
		if (tids != NULL)
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
	filter->tids	  = tids;
	filter->count	  = count;
	filter->via_index = used_index;
	if (used_index)
		strlcpy(filter->idxname, idxname, NAMEDATALEN);
	else
		filter->idxname[0] = '\0';

	return filter;
}

void
tp_cleanup_query_facets(void)
{
	tp_facet_free_pending_value();
	tp_pending_facet.is_valid = false;
	tp_active_facet			  = NULL;
}

/*
 * Drop only the pending (planner-stashed) spec, leaving any active scan
 * filter alone. Called at the start of each BM25-path costing and at
 * ExecutorEnd so a spec stashed while costing a path that is never consumed
 * (the path lost, or was only EXPLAINed) cannot leak into a later statement
 * on the same index.
 */
void
tp_reset_pending_facet(void)
{
	tp_facet_free_pending_value();
	tp_pending_facet.is_valid = false;
}

/* Whether a planner-stashed spec is currently pending. */
bool
tp_pending_facet_valid(void)
{
	return tp_pending_facet.is_valid;
}

/* The BM25 index OID the pending spec targets (InvalidOid if none). */
Oid
tp_pending_facet_index_oid(void)
{
	return tp_pending_facet.is_valid ? tp_pending_facet.bm25_index_oid
									 : InvalidOid;
}

/*
 * True if the given scan's filter quals contain the pending spec's clause:
 * an OpExpr on the spec's operator with the spec's column (Var of scanrelid)
 * on one side and the spec's exact constant on the other. Used by the
 * ExecutorRun sanitizer to confirm a scan genuinely carries this facet before
 * the AM (which keys only by index OID) is allowed to consume it.
 */
bool
tp_pending_facet_qual_matches(Index scanrelid, List *qual)
{
	ListCell *lc;

	if (!tp_pending_facet.is_valid)
		return false;

	foreach (lc, qual)
	{
		Node   *clause = (Node *)lfirst(lc);
		OpExpr *op;
		Node   *leftop;
		Node   *rightop;
		Var	   *var;
		Const  *con;

		if (!IsA(clause, OpExpr))
			continue;

		op = (OpExpr *)clause;
		if (op->opno != tp_pending_facet.opno || list_length(op->args) != 2)
			continue;

		leftop	= strip_implicit_coercions((Node *)linitial(op->args));
		rightop = strip_implicit_coercions((Node *)lsecond(op->args));

		if (IsA(leftop, Var) && IsA(rightop, Const))
		{
			var = (Var *)leftop;
			con = (Const *)rightop;
		}
		else if (IsA(leftop, Const) && IsA(rightop, Var))
		{
			con = (Const *)leftop;
			var = (Var *)rightop;
		}
		else
			continue;

		if ((Index)var->varno != scanrelid ||
			var->varattno != tp_pending_facet.attno)
			continue;

		if (con->constisnull || con->consttype != tp_pending_facet.consttype)
			continue;

		if (datumIsEqual(
					con->constvalue,
					tp_pending_facet.value,
					tp_pending_facet.typbyval,
					tp_pending_facet.typlen))
			return true;
	}

	return false;
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
