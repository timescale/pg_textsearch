/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * vacuum.c - BM25 index vacuum and maintenance operations
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/generic_xlog.h>
#include <access/heapam.h>
#include <catalog/index.h>
#include <catalog/namespace.h>
#include <commands/progress.h>
#include <commands/vacuum.h>
#include <executor/executor.h>
#include <fmgr.h>
#include <lib/dshash.h>
#include <nodes/execnodes.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <tsearch/ts_utils.h>
#include <utils/builtins.h>
#include <utils/fmgrprotos.h>
#include <utils/regproc.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>

#include "access/am.h"
#include "access/build_context.h"
#include "index/metapage.h"
#include "index/state.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "segment/alive_bitset.h"
#include "segment/io.h"
#include "segment/merge.h"
#include "segment/segment.h"

/*
 * Per-segment state for VACUUM dead tuple tracking.
 * Tracks which segments contain dead CTIDs without storing the
 * CTIDs themselves.
 */
typedef struct TpVacuumSegmentInfo
{
	BlockNumber root_block;
	BlockNumber next_segment;
	uint32		level;
	uint32		num_docs;	  /* segment header num_docs */
	uint64		total_tokens; /* segment header total_tokens */
	uint32	   *dead_doc_ids; /* Array of dead doc_ids */
	uint32		dead_count;
	bool		affected;
	bool		is_v5; /* true if segment has alive bitset */
} TpVacuumSegmentInfo;

/*
 * Sum segment.alive_count across all on-disk segments.  Used by
 * tp_vacuumcleanup to set stats->num_index_tuples: reltuples must
 * reflect the live-doc count, which may be strictly less than
 * metap->total_docs (that tracks Σ segment.num_docs, which for V5
 * segments includes bitset-dead docs).
 *
 * For V5 segments the count is in the header; pre-V5 segments
 * have no alive-bitset so their alive count equals num_docs.
 */
static uint64
tp_count_live_docs(Relation index, TpIndexMetaPage metap)
{
	uint64 alive = 0;

	for (int level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber seg = metap->level_heads[level];

		while (seg != InvalidBlockNumber)
		{
			TpSegmentReader *reader = tp_segment_open(index, seg);

			if (!reader || !reader->header)
			{
				if (reader)
					tp_segment_close(reader);
				break;
			}

			alive += (reader->header->alive_bitset_offset > 0)
						   ? reader->header->alive_count
						   : reader->header->num_docs;
			seg = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}
	return alive;
}

/*
 * Apply the invariant total_docs = Σ segment.num_docs (and its
 * total_len counterpart) after Phase 3 has rebuilt or dropped
 * segments.  Decrements both the shared-memory atomic and the
 * on-disk metapage under the metapage buffer exclusive lock so a
 * concurrent tp_sync_metapage_stats (which acquires the same lock)
 * cannot observe a half-applied state or re-inflate the metapage
 * from the atomic between the two decrements.  See
 * TpIndexMetaPageData.total_docs in metapage.h for the invariant.
 */
static void
tp_apply_vacuum_shrinkage(
		Relation		   index,
		TpLocalIndexState *index_state,
		uint64			   docs_shrinkage,
		uint64			   tokens_shrinkage)
{
	Buffer			  mbuf;
	GenericXLogState *xlog_state;
	Page			  mpage;
	TpIndexMetaPage	  mp;

	if (docs_shrinkage == 0 && tokens_shrinkage == 0)
		return;

	mbuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);

	if (index_state != NULL && index_state->shared != NULL)
	{
		/*
		 * Clamp symmetrically with the metapage write below so a
		 * shrinkage that exceeds the current atomic (reachable via
		 * pre-fix L0 headers carrying inflated total_tokens) can't
		 * wrap the u64 atomic.  A wrap would propagate back to disk
		 * on the next tp_sync_metapage_stats, which writes the
		 * atomic into metap unconditionally.
		 */
		if (docs_shrinkage > 0)
		{
			uint32 cur_docs = pg_atomic_read_u32(
					&index_state->shared->total_docs);
			uint32 sub_docs = (docs_shrinkage > (uint64)cur_docs)
									? cur_docs
									: (uint32)docs_shrinkage;

			if (sub_docs > 0)
				pg_atomic_fetch_sub_u32(
						&index_state->shared->total_docs, sub_docs);
		}
		if (tokens_shrinkage > 0)
		{
			uint64 cur_len = pg_atomic_read_u64(
					&index_state->shared->total_len);
			uint64 sub_len = Min(cur_len, tokens_shrinkage);

			if (sub_len > 0)
				pg_atomic_fetch_sub_u64(
						&index_state->shared->total_len, sub_len);
		}
	}

	xlog_state = GenericXLogStart(index);
	mpage	   = GenericXLogRegisterBuffer(xlog_state, mbuf, 0);
	mp		   = (TpIndexMetaPage)PageGetContents(mpage);

	mp->total_docs = (mp->total_docs >= docs_shrinkage)
						   ? mp->total_docs - docs_shrinkage
						   : 0;
	mp->total_len  = (mp->total_len >= tokens_shrinkage)
						   ? mp->total_len - tokens_shrinkage
						   : 0;

	GenericXLogFinish(xlog_state);
	UnlockReleaseBuffer(mbuf);
}

/*
 * Spill memtable to an L0 segment.  Caller passes a minimum posting
 * count below which the spill is a no-op — used by VACUUM cleanup
 * and the shutdown hook to avoid producing runt L0 segments on
 * lightly-loaded indexes.  The pre-lock read is a fast bailout; if
 * it races with an insert, the worst case is a harmless no-op
 * inside tp_write_segment.
 */
void
tp_spill_memtable_if_needed(
		Relation index, TpLocalIndexState *index_state, uint64 min_postings)
{
	TpMemtable *memtable;
	BlockNumber segment_root;

	if (!index_state || !index_state->shared)
		return;

	memtable = get_memtable(index_state);
	if (!memtable ||
		pg_atomic_read_u64(&memtable->total_postings) < min_postings)
		return;

	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

	segment_root = tp_write_segment(index_state, index);
	if (segment_root != InvalidBlockNumber)
	{
		tp_clear_memtable(index_state);
		tp_clear_docid_pages(index);
		tp_link_l0_chain_head(index, segment_root);
		tp_sync_metapage_stats(index, index_state);
		tp_maybe_compact_level(index, 0);
	}

	tp_release_index_lock(index_state);
}

/*
 * Walk all segment docmaps and call the callback for each CTID.
 * Returns an array of TpVacuumSegmentInfo with affected flags set.
 * *num_segments_out receives the total segment count.
 */
static TpVacuumSegmentInfo *
tp_vacuum_identify_affected(
		Relation				index,
		TpIndexMetaPage			metap,
		IndexBulkDeleteCallback callback,
		void				   *callback_state,
		int					   *num_segments_out,
		int64				   *total_dead_out)
{
	TpVacuumSegmentInfo *segments;
	int					 capacity	= 32;
	int					 count		= 0;
	int64				 total_dead = 0;

	segments = palloc(capacity * sizeof(TpVacuumSegmentInfo));

	for (int level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber seg = metap->level_heads[level];

		while (seg != InvalidBlockNumber)
		{
			TpSegmentReader *reader;
			uint32			 seg_dead = 0;

			reader = tp_segment_open_ex(index, seg, true);
			if (!reader || !reader->header)
			{
				if (reader)
					tp_segment_close(reader);
				break;
			}

			/* Check each CTID against the callback */
			{
				uint32 *dead_ids   = NULL;
				uint32	dead_cap   = 0;
				bool	has_bitset = (reader->header->alive_bitset_offset > 0);

				for (uint32 i = 0; i < reader->header->num_docs; i++)
				{
					ItemPointerData ctid;

					/*
					 * Skip docs already marked dead in the alive
					 * bitset.  Without this, CTID reuse after a
					 * previous VACUUM would double-count dead docs
					 * and corrupt total_docs in the metapage.
					 */
					if (has_bitset && !tp_segment_is_alive(reader, i))
						continue;

					tp_segment_lookup_ctid(reader, i, &ctid);
					if (ItemPointerIsValid(&ctid) &&
						callback(&ctid, callback_state))
					{
						if (seg_dead >= dead_cap)
						{
							dead_cap = (dead_cap == 0) ? 64 : dead_cap * 2;
							dead_ids = dead_ids
											 ? repalloc(
													   dead_ids,
													   dead_cap *
															   sizeof(uint32))
											 : palloc(dead_cap *
													  sizeof(uint32));
						}
						dead_ids[seg_dead] = i;
						seg_dead++;
					}
				}

				/* Record segment info */
				if (count >= capacity)
				{
					capacity *= 2;
					segments = repalloc(
							segments, capacity * sizeof(TpVacuumSegmentInfo));
				}

				segments[count].root_block	 = seg;
				segments[count].next_segment = reader->header->next_segment;
				segments[count].level		 = level;
				segments[count].num_docs	 = reader->header->num_docs;
				segments[count].total_tokens = reader->header->total_tokens;
				segments[count].dead_doc_ids = dead_ids;
				segments[count].dead_count	 = seg_dead;
				segments[count].affected	 = (seg_dead > 0);
				segments[count].is_v5 =
						(reader->header->alive_bitset_offset > 0);
			}

			total_dead += seg_dead;
			count++;

			seg = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}

	*num_segments_out = count;
	*total_dead_out	  = total_dead;
	return segments;
}

/*
 * Rebuild a single segment, excluding dead CTIDs.
 *
 * Reads the segment's docmap, calls the callback for each CTID,
 * fetches live heap tuples, tokenizes them, and writes a new
 * segment via TpBuildContext.
 *
 * Returns the new segment's root block, or InvalidBlockNumber if
 * all docs were dead (segment should be removed from chain).
 */
static BlockNumber
tp_vacuum_rebuild_segment(
		Relation				index,
		Relation				heap,
		BlockNumber				old_root,
		uint32 level			pg_attribute_unused(),
		IndexBulkDeleteCallback callback,
		void				   *callback_state,
		uint64				   *new_total_docs,
		uint64				   *new_total_len)
{
	TpSegmentReader *reader;
	TpBuildContext	*build_ctx;
	BlockNumber		 new_root;
	Oid				 text_config_oid;
	IndexInfo		*indexInfo;
	EState			*estate;
	ExprContext		*econtext;
	TupleTableSlot	*eval_slot;
	Datum			 idx_values[INDEX_MAX_KEYS];
	bool			 idx_isnull[INDEX_MAX_KEYS];
	MemoryContext	 per_doc_ctx;
	MemoryContext	 old_ctx;
	uint64			 docs_added = 0;
	uint64			 len_added	= 0;

	/* Get text config from metapage */
	{
		TpIndexMetaPage mp = tp_get_metapage(index);

		text_config_oid = mp->text_config_oid;
		pfree(mp);
	}

	/* Open segment with CTID preloading */
	reader = tp_segment_open_ex(index, old_root, true);
	if (!reader || !reader->header)
	{
		if (reader)
			tp_segment_close(reader);
		*new_total_docs = 0;
		if (new_total_len)
			*new_total_len = 0;
		return InvalidBlockNumber;
	}

	/* Set up expression evaluation for index */
	indexInfo = BuildIndexInfo(index);
	estate	  = CreateExecutorState();
	econtext  = GetPerTupleExprContext(estate);
	eval_slot = MakeSingleTupleTableSlot(
			RelationGetDescr(heap), &TTSOpsBufferHeapTuple);

	if (indexInfo->ii_Predicate != NIL)
		indexInfo->ii_PredicateState =
				ExecPrepareQual(indexInfo->ii_Predicate, estate);

	/* Create build context (no budget limit for VACUUM rebuild) */
	build_ctx = tp_build_context_create(0);

	per_doc_ctx = AllocSetContextCreate(
			CurrentMemoryContext,
			"VACUUM rebuild per-doc",
			ALLOCSET_DEFAULT_SIZES);

	/* Iterate docmap, skip dead, fetch+tokenize live */
	for (uint32 i = 0; i < reader->header->num_docs; i++)
	{
		ItemPointerData ctid;
		HeapTupleData	tuple_data;
		HeapTuple		tuple	 = &tuple_data;
		Buffer			heap_buf = InvalidBuffer;
		bool			valid;
		text		   *document_text;
		Datum			tsvector_datum;
		TSVector		tsvector;
		char		  **terms;
		int32		   *frequencies;
		uint16		  **positions;
		int				term_count;
		int				doc_length;

		tp_segment_lookup_ctid(reader, i, &ctid);
		if (!ItemPointerIsValid(&ctid))
			continue;

		/* Skip dead CTIDs */
		if (callback(&ctid, callback_state))
			continue;

		/* Fetch heap tuple */
		tuple->t_self = ctid;
		valid		  = heap_fetch(heap, SnapshotAny, tuple, &heap_buf, true);
		if (!valid)
		{
			if (heap_buf != InvalidBuffer)
				ReleaseBuffer(heap_buf);
			continue;
		}

		/* Evaluate index expression */
		ExecStoreBufferHeapTuple(tuple, eval_slot, heap_buf);
		econtext->ecxt_scantuple = eval_slot;
		FormIndexDatum(indexInfo, eval_slot, estate, idx_values, idx_isnull);

		if (idx_isnull[0])
		{
			ExecClearTuple(eval_slot);
			ResetExprContext(econtext);
			ReleaseBuffer(heap_buf);
			continue;
		}

		/* Check partial index predicate */
		if (indexInfo->ii_Predicate != NIL &&
			!ExecQual(indexInfo->ii_PredicateState, econtext))
		{
			ExecClearTuple(eval_slot);
			ResetExprContext(econtext);
			ReleaseBuffer(heap_buf);
			continue;
		}

		/* Tokenize in per-doc context (includes detoasting) */
		old_ctx = MemoryContextSwitchTo(per_doc_ctx);

		document_text = DatumGetTextPP(idx_values[0]);

		tsvector_datum = DirectFunctionCall2Coll(
				to_tsvector_byid,
				InvalidOid,
				ObjectIdGetDatum(text_config_oid),
				PointerGetDatum(document_text));
		tsvector = DatumGetTSVector(tsvector_datum);

		doc_length = tp_extract_terms_from_tsvector(
				tsvector, &terms, &frequencies, &positions, &term_count);

		MemoryContextSwitchTo(old_ctx);

		if (term_count > 0)
		{
			tp_build_context_add_document(
					build_ctx,
					terms,
					frequencies,
					positions,
					term_count,
					doc_length,
					&ctid);
			docs_added++;
			len_added += doc_length;
		}

		MemoryContextReset(per_doc_ctx);
		ExecClearTuple(eval_slot);
		ResetExprContext(econtext);
		ReleaseBuffer(heap_buf);
	}

	ExecDropSingleTupleTableSlot(eval_slot);
	FreeExecutorState(estate);
	tp_segment_close(reader);

	/* Write new segment if any docs survived */
	if (build_ctx->num_docs > 0)
		new_root = tp_write_segment_from_build_ctx(build_ctx, index);
	else
		new_root = InvalidBlockNumber;

	tp_build_context_destroy(build_ctx);
	MemoryContextDelete(per_doc_ctx);

	*new_total_docs = docs_added;
	if (new_total_len)
		*new_total_len = len_added;
	return new_root;
}

/*
 * Replace or remove a segment in a level's chain.
 *
 * If new_root is valid, replaces old_root with new_root in the
 * chain. If new_root is InvalidBlockNumber, removes old_root from
 * the chain entirely.
 *
 * Updates the metapage level_heads/level_counts as needed.
 * Frees old segment pages.
 */
static void
tp_vacuum_replace_segment(
		Relation	index,
		uint32		level,
		BlockNumber old_root,
		BlockNumber new_root,
		BlockNumber prev_root)
{
	Buffer		 metabuf;
	BlockNumber *old_pages;
	uint32		 old_page_count;
	BlockNumber	 old_next;

	/*
	 * Read old segment's next_segment pointer before we free it.
	 * We need this to fix up chain pointers.
	 */
	{
		TpSegmentReader *old_reader;

		old_reader = tp_segment_open(index, old_root);
		old_next   = old_reader->header->next_segment;
		tp_segment_close(old_reader);
	}

	/* Collect old segment pages for freeing */
	old_page_count = tp_segment_collect_pages(index, old_root, &old_pages);

	/*
	 * Update chain pointers atomically via GenericXLog.
	 * Up to 3 buffers: metapage, new segment, prev segment.
	 */
	{
		GenericXLogState *xlog_state;
		Page			  meta_copy;
		TpIndexMetaPage	  meta_ptr;
		Buffer			  new_buf  = InvalidBuffer;
		Buffer			  prev_buf = InvalidBuffer;

		metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

		xlog_state = GenericXLogStart(index);
		meta_copy  = GenericXLogRegisterBuffer(xlog_state, metabuf, 0);
		meta_ptr   = (TpIndexMetaPage)PageGetContents(meta_copy);

		/* Set new segment's next_segment and level */
		if (new_root != InvalidBlockNumber)
		{
			Page			 new_page;
			TpSegmentHeader *new_header;

			new_buf = ReadBuffer(index, new_root);
			LockBuffer(new_buf, BUFFER_LOCK_EXCLUSIVE);
			new_page = GenericXLogRegisterBuffer(xlog_state, new_buf, 0);
			/* Ensure pd_lower covers content for GenericXLog */
			((PageHeader)new_page)->pd_lower = BLCKSZ;
			new_header = (TpSegmentHeader *)PageGetContents(new_page);
			new_header->next_segment = old_next;
			new_header->level		 = level;
		}

		if (prev_root == InvalidBlockNumber)
		{
			/* old_root was the head of this level */
			if (new_root != InvalidBlockNumber)
				meta_ptr->level_heads[level] = new_root;
			else
			{
				meta_ptr->level_heads[level] = old_next;
				meta_ptr->level_counts[level]--;
			}
		}
		else
		{
			/* Update prev's next_segment pointer. */
			Page		prev_page;
			char	   *prev_content;
			uint32		prev_version;
			BlockNumber new_next;

			prev_buf = ReadBuffer(index, prev_root);
			LockBuffer(prev_buf, BUFFER_LOCK_EXCLUSIVE);
			prev_page = GenericXLogRegisterBuffer(xlog_state, prev_buf, 0);
			/* Ensure pd_lower covers content for GenericXLog */
			((PageHeader)prev_page)->pd_lower = BLCKSZ;

			new_next = (new_root != InvalidBlockNumber) ? new_root : old_next;

			/*
			 * The previous segment may still be on an older on-disk
			 * format.  V3 uses uint32 offsets and places next_segment
			 * at byte 28; V4/V5 use uint64 offsets with 4 bytes of
			 * padding before data_size, placing next_segment at byte
			 * 36.  Write at the offset matching the on-disk version
			 * so we don't clobber adjacent fields.  magic/version
			 * live at the same bytes in every version, so reading
			 * version via the V5 struct is safe.
			 */
			prev_content = PageGetContents(prev_page);
			prev_version = ((TpSegmentHeader *)prev_content)->version;
			if (prev_version <= TP_SEGMENT_FORMAT_VERSION_3)
				((TpSegmentHeaderV3 *)prev_content)->next_segment = new_next;
			else
				((TpSegmentHeader *)prev_content)->next_segment = new_next;

			if (new_root == InvalidBlockNumber)
				meta_ptr->level_counts[level]--;
		}

		GenericXLogFinish(xlog_state);
		if (BufferIsValid(prev_buf))
			UnlockReleaseBuffer(prev_buf);
		if (BufferIsValid(new_buf))
			UnlockReleaseBuffer(new_buf);
		UnlockReleaseBuffer(metabuf);
	}

	/* Free old segment pages */
	if (old_pages && old_page_count > 0)
		tp_segment_free_pages(index, old_pages, old_page_count);
	if (old_pages)
		pfree(old_pages);

	IndexFreeSpaceMapVacuum(index);
}

/*
 * Apply dead doc marks to a V5 segment's alive bitset in-place.
 *
 * Modifies bitset pages directly via GenericXLog — only pages
 * containing dead docs are read and written.  No bulk allocation.
 * Returns the new alive_count, or 0 if all docs are dead
 * (caller should drop the segment).
 */
static uint32
tp_vacuum_mark_dead(
		Relation	index,
		BlockNumber root_block,
		uint32	   *dead_doc_ids,
		uint32		dead_count)
{
	TpSegmentReader *reader;
	uint32			 alive;

	reader = tp_segment_open_ex(index, root_block, false);
	if (!reader || !reader->header)
	{
		if (reader)
			tp_segment_close(reader);
		return 0;
	}

	alive = tp_alive_bitset_mark_dead_inplace(
			index, reader, dead_doc_ids, dead_count);

	tp_segment_close(reader);

	return alive;
}

/*
 * Bulk delete callback for vacuum and CREATE INDEX CONCURRENTLY
 *
 * Four-phase approach:
 * 1. Spill memtable to segments (all data in uniform format)
 * 2. Identify segments containing dead CTIDs (O(segments) memory)
 * 3. Mark dead docs or rebuild affected segments
 * 4. Update metapage statistics
 *
 * Also called during CREATE INDEX CONCURRENTLY validation with a
 * callback that returns false for all CTIDs (just collecting TIDs).
 * That path is a no-op: no dead CTIDs means no segments are marked
 * affected, so we skip directly to returning stats.
 */
IndexBulkDeleteResult *
tp_bulkdelete(
		IndexVacuumInfo		   *info,
		IndexBulkDeleteResult  *stats,
		IndexBulkDeleteCallback callback,
		void				   *callback_state)
{
	TpIndexMetaPage		 metap;
	TpLocalIndexState	*index_state;
	TpVacuumSegmentInfo *segments;
	int					 num_segments;
	int64				 total_dead;

	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	metap = tp_get_metapage(info->index);
	if (!metap)
	{
		stats->num_pages		= 1;
		stats->num_index_tuples = 0;
		stats->tuples_removed	= 0;
		stats->pages_deleted	= 0;
		elog(WARNING,
			 "Tapir bulkdelete: couldn't read metapage for "
			 "index %s",
			 RelationGetRelationName(info->index));
		return stats;
	}

	if (callback == NULL)
	{
		stats->num_pages		= 1;
		stats->num_index_tuples = (double)metap->total_docs;
		stats->tuples_removed	= 0;
		stats->pages_deleted	= 0;
		pfree(metap);
		return stats;
	}

	/*
	 * Phase 1: Spill memtable so all data is in segments.  Pass
	 * min_postings=1 to preserve existing "spill anything non-empty"
	 * behavior for the bulkdelete path.
	 */
	index_state = tp_get_local_index_state(RelationGetRelid(info->index));
	if (index_state != NULL)
		tp_spill_memtable_if_needed(info->index, index_state, 1);

	/* Re-read metapage after spill */
	pfree(metap);
	metap = tp_get_metapage(info->index);
	if (!metap)
	{
		stats->num_pages		= 1;
		stats->num_index_tuples = 0;
		stats->tuples_removed	= 0;
		return stats;
	}

	/* Phase 2: Identify affected segments */
	segments = tp_vacuum_identify_affected(
			info->index,
			metap,
			callback,
			callback_state,
			&num_segments,
			&total_dead);

	if (total_dead == 0)
	{
		/* No dead tuples -- nothing to rebuild */
		stats->num_pages		= 1;
		stats->num_index_tuples = (double)metap->total_docs;
		stats->tuples_removed	= 0;
		stats->pages_deleted	= 0;
		pfree(metap);
		pfree(segments);
		return stats;
	}

	elog(DEBUG1,
		 "Tapir VACUUM: %lld dead tuples across %d segments",
		 (long long)total_dead,
		 num_segments);

	/*
	 * Phase 3: Mark dead docs or rebuild affected segments.  Track
	 * segment-header shrinkage so we can restore the invariant
	 * total_docs = Σ segment.num_docs (see metapage.h).  V5 bitset
	 * flips that leave survivors do not change the segment header's
	 * num_docs / total_tokens, so they contribute zero shrinkage.
	 */
	{
		uint64 docs_shrinkage	= 0;
		uint64 tokens_shrinkage = 0;

		for (int level = 0; level < TP_MAX_LEVELS; level++)
		{
			BlockNumber prev = InvalidBlockNumber;

			for (int i = 0; i < num_segments; i++)
			{
				if ((int)segments[i].level != level)
					continue;

				if (segments[i].affected)
				{
					if (segments[i].is_v5)
					{
						/*
						 * V5 segment: flip bits in alive
						 * bitset.
						 */
						uint32 alive = tp_vacuum_mark_dead(
								info->index,
								segments[i].root_block,
								segments[i].dead_doc_ids,
								segments[i].dead_count);

						if (alive == 0)
						{
							/*
							 * All docs dead -- drop segment.
							 */
							tp_vacuum_replace_segment(
									info->index,
									level,
									segments[i].root_block,
									InvalidBlockNumber,
									prev);
							docs_shrinkage += segments[i].num_docs;
							tokens_shrinkage += segments[i].total_tokens;
							/* prev stays the same */
						}
						else
						{
							prev = segments[i].root_block;
						}
					}
					else
					{
						/*
						 * Pre-V5 segment: rebuild into V5.
						 */
						BlockNumber new_root;
						uint64		new_docs   = 0;
						uint64		new_tokens = 0;

						new_root = tp_vacuum_rebuild_segment(
								info->index,
								info->heaprel,
								segments[i].root_block,
								level,
								callback,
								callback_state,
								&new_docs,
								&new_tokens);

						tp_vacuum_replace_segment(
								info->index,
								level,
								segments[i].root_block,
								new_root,
								prev);

						/*
						 * Clamp to zero: new_tokens is a raw
						 * re-tokenization sum, while
						 * segments[i].total_tokens comes from a
						 * pre-V5 header that may have been written
						 * with a quantized (merge) or cumulative
						 * (pre-fix L0 spill) value.  Underflow here
						 * would wrap into a huge positive shrinkage
						 * before the tp_apply_vacuum_shrinkage clamp
						 * sees it.  num_docs has no comparable
						 * corruption path, but clamping both keeps
						 * the code symmetric.
						 */
						if (segments[i].num_docs > new_docs)
							docs_shrinkage += segments[i].num_docs - new_docs;
						if (segments[i].total_tokens > new_tokens)
							tokens_shrinkage += segments[i].total_tokens -
												new_tokens;

						if (new_root != InvalidBlockNumber)
							prev = new_root;
					}
				}
				else
				{
					prev = segments[i].root_block;
				}
			}
		}

		tp_apply_vacuum_shrinkage(
				info->index, index_state, docs_shrinkage, tokens_shrinkage);
	}

	/*
	 * tp_vacuumcleanup will set num_index_tuples to the actual live
	 * count; only tuples_removed needs to carry through from here.
	 */
	stats->num_pages	  = 1;
	stats->tuples_removed = (double)total_dead;
	stats->pages_deleted  = 0;

	pfree(metap);
	for (int i = 0; i < num_segments; i++)
	{
		if (segments[i].dead_doc_ids)
			pfree(segments[i].dead_doc_ids);
	}
	pfree(segments);

	return stats;
}

/*
 * Vacuum/cleanup the BM25 index
 */
IndexBulkDeleteResult *
tp_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	TpIndexMetaPage	   metap;
	TpLocalIndexState *index_state;

	/* Initialize stats if not provided */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	/*
	 * Spill the memtable so the docid chain doesn't outlive a
	 * server restart.  Insert-only tables skip ambulkdelete (no
	 * dead tuples), so without this call nothing would spill and
	 * the first query after the next start would be slow.  Skip
	 * under TP_MIN_SPILL_POSTINGS — that few docs is faster to
	 * replay from heap on restart than a runt L0 segment would be
	 * to compact away.
	 */
	index_state = tp_get_local_index_state(RelationGetRelid(info->index));
	if (index_state != NULL)
		tp_spill_memtable_if_needed(
				info->index, index_state, TP_MIN_SPILL_POSTINGS);

	/* Get current index statistics from metapage */
	metap = tp_get_metapage(info->index);
	if (metap)
	{
		stats->num_pages = 1;
		/*
		 * reltuples tracks live docs, which can be less than
		 * metap->total_docs because V5 bitset flips with survivors
		 * reduce alive_count without changing segment.num_docs.  Sum
		 * alive_count across segments for an accurate live count
		 * regardless of whether tp_bulkdelete ran or this is a
		 * no-deletes maintenance round.
		 *
		 * Hold the per-index LWLock in shared mode across the walk
		 * so concurrent spills / compactions (which take
		 * LW_EXCLUSIVE to mutate level_heads and free segment
		 * pages via the FSM) can't recycle a page we're about to
		 * tp_segment_open.
		 */
		if (index_state != NULL)
			tp_acquire_index_lock(index_state, LW_SHARED);
		stats->num_index_tuples = (double)
				tp_count_live_docs(info->index, metap);
		if (index_state != NULL)
			tp_release_index_lock(index_state);

		if (stats->pages_deleted == 0 && stats->tuples_removed == 0)
			stats->pages_free = 0;

		pfree(metap);
	}
	else
	{
		elog(WARNING,
			 "Tapir vacuum cleanup: couldn't read metapage "
			 "for index %s",
			 RelationGetRelationName(info->index));

		if (stats->num_pages == 0 && stats->num_index_tuples == 0)
		{
			stats->num_pages		= 1;
			stats->num_index_tuples = 0;
		}
	}

	return stats;
}
