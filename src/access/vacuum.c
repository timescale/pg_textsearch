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
#include <access/transam.h>
#include <catalog/index.h>
#include <catalog/namespace.h>
#include <commands/progress.h>
#include <commands/vacuum.h>
#include <executor/executor.h>
#include <fmgr.h>
#include <lib/dshash.h>
#include <miscadmin.h>
#include <nodes/execnodes.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <storage/procarray.h>
#include <tsearch/ts_utils.h>
#include <utils/builtins.h>
#include <utils/fmgrprotos.h>
#include <utils/hsearch.h>
#include <utils/regproc.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>
#include <utils/timestamp.h>

#include "access/am.h"
#include "access/build_context.h"
#include "index/freepage.h"
#include "index/metapage.h"
#include "index/state.h"
#include "memtable/page.h"
#include "segment/alive_bitset.h"
#include "segment/compaction.h"
#include "segment/graph_snapshot.h"
#include "segment/io.h"
#include "segment/merge.h"
#include "segment/segment.h"
#include "segment/tombstone.h"

/*
 * Per-segment state for VACUUM dead tuple tracking.
 * Tracks which segments contain dead CTIDs without storing the
 * CTIDs themselves.
 */
typedef struct TpVacuumSegmentInfo
{
	BlockNumber root_block;
	uint32		level;
	uint32		num_docs;	  /* segment header num_docs */
	uint64		total_tokens; /* segment header total_tokens */
	uint32	   *dead_doc_ids; /* Array of dead doc_ids */
	uint32		dead_count;
	bool		affected;
	bool		is_v5;		   /* true if segment has alive bitset */
	bool		already_empty; /* V5 header already has no live docs */
} TpVacuumSegmentInfo;

static void
tp_debug_vacuum_pause_during_identification(Relation index)
{
	TimestampTz deadline;

	if (tp_debug_compaction_pause_source_estimate_ms <= 0)
		return;

	ereport(LOG,
			(errmsg("pg_textsearch VACUUM pause during identification for "
					"index %u backend %d",
					RelationGetRelid(index),
					MyProcPid)));
	deadline = TimestampTzPlusMilliseconds(
			GetCurrentTimestamp(),
			tp_debug_compaction_pause_source_estimate_ms);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (GetCurrentTimestamp() >= deadline)
			break;
		pg_usleep(10000L);
	}
	ereport(LOG,
			(errmsg("pg_textsearch VACUUM resume after identification for "
					"index %u backend %d",
					RelationGetRelid(index),
					MyProcPid)));
}

/*
 * Convert a 32-bit xid (known to be in the allowable range when
 * nextFullXid was current) into a FullTransactionId.
 */
static inline FullTransactionId
tp_full_xid_from_allowable_at(FullTransactionId nextFullXid, TransactionId xid)
{
#if PG_VERSION_NUM >= 170003
	return FullTransactionIdFromAllowableAt(nextFullXid, xid);
#else
	uint32 epoch;

	if (!TransactionIdIsNormal(xid))
		return FullTransactionIdFromEpochAndXid(0, xid);

	Assert(TransactionIdPrecedesOrEquals(
			xid, XidFromFullTransactionId(nextFullXid)));

	epoch = EpochFromFullTransactionId(nextFullXid);
	if (xid > XidFromFullTransactionId(nextFullXid))
	{
		Assert(epoch != 0);
		epoch--;
	}

	return FullTransactionIdFromEpochAndXid(epoch, xid);
#endif
}

FullTransactionId
tp_reclaim_horizon(Relation heaprel)
{
	TransactionId oldest = GetOldestNonRemovableTransactionId(heaprel);

	return tp_full_xid_from_allowable_at(ReadNextFullTransactionId(), oldest);
}

/*
 * Build a hash set of block numbers reachable from the current
 * memtable chain (metap->memtable_head_blkno).  Used by
 * tp_reclaim_dead_memtable_pages to avoid freeing pages that are
 * still in the live chain — a crash-safety guard against the
 * scenario where a crash between tp_spill_finalize and
 * tp_memtable_mark_chain_dead leaves pages reachable but stamped
 * DEAD once global xmin advances.
 *
 * Walks both the main chain (via next_block) and any fragment
 * continuation sub-chains.  Caller must hash_destroy() the result.
 */
static HTAB *
tp_collect_reachable_chain_blocks(Relation indexrel)
{
	HASHCTL			info;
	HTAB		   *reachable;
	TpIndexMetaPage metap;
	BlockNumber		cur;

	memset(&info, 0, sizeof(info));
	info.keysize   = sizeof(BlockNumber);
	info.entrysize = sizeof(BlockNumber);
	info.hcxt	   = CurrentMemoryContext;
	reachable	   = hash_create(
			 "reachable chain blocks",
			 128,
			 &info,
			 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	metap = tp_get_metapage(indexrel);
	if (metap == NULL)
		return reachable;

	cur = metap->memtable_head_blkno;
	pfree(metap);

	while (cur != InvalidBlockNumber)
	{
		Buffer				  buf;
		Page				  page;
		TpMemtablePageHeader *hdr;
		TpMemtableRecord	 *rec;
		BlockNumber			  next;
		bool				  found;

		CHECK_FOR_INTERRUPTS();

		hash_search(reachable, &cur, HASH_ENTER, &found);

		buf = ReadBuffer(indexrel, cur);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		if (!tp_memtable_page_is_valid(page))
		{
			UnlockReleaseBuffer(buf);
			break;
		}

		hdr	 = tp_memtable_page_header(page);
		next = hdr->next_block;
		rec	 = tp_memtable_page_first(page);

		/*
		 * If this page has a fragment record, its next_block points
		 * to the first continuation page.  Walk the continuation
		 * chain and add those blocks too, then resume from the
		 * first non-continuation block.
		 */
		if (rec != NULL &&
			(rec->flags & TP_MEMTABLE_RECORD_FLAG_FRAGMENT) != 0)
		{
			BlockNumber cont = next;

			while (cont != InvalidBlockNumber)
			{
				Buffer				  cont_buf;
				Page				  cont_page;
				TpMemtablePageHeader *cont_hdr;

				hash_search(reachable, &cont, HASH_ENTER, &found);

				cont_buf = ReadBuffer(indexrel, cont);
				LockBuffer(cont_buf, BUFFER_LOCK_SHARE);
				cont_page = BufferGetPage(cont_buf);

				if (!tp_memtable_page_is_valid(cont_page) ||
					!tp_memtable_page_is_continuation(cont_page))
				{
					next = cont;
					UnlockReleaseBuffer(cont_buf);
					break;
				}

				cont_hdr = tp_memtable_page_header(cont_page);
				cont	 = cont_hdr->next_block;
				UnlockReleaseBuffer(cont_buf);
			}
			/*
			 * If the inner loop exhausted (cont == Invalid), next
			 * stays as the last continuation's next_block, which
			 * is Invalid — outer loop terminates correctly.
			 */
			if (cont == InvalidBlockNumber)
				next = InvalidBlockNumber;
		}

		UnlockReleaseBuffer(buf);
		cur = next;
	}

	return reachable;
}

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
tp_count_live_docs(
		Relation index, const TpSegmentGraphSnapshot *segment_snapshot)
{
	uint64 alive = 0;

	for (int level = 0; level < TP_MAX_LEVELS; level++)
	{
		const BlockNumber *roots;
		uint32			   root_count;

		roots = tp_segment_graph_snapshot_level(
				segment_snapshot, level, &root_count);
		for (uint32 root_idx = 0; root_idx < root_count; root_idx++)
		{
			BlockNumber		 seg	= roots[root_idx];
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
			tp_segment_close(reader);
		}
	}
	return alive;
}

/*
 * Walk all segment docmaps and call the callback for each CTID.
 * Returns an array of TpVacuumSegmentInfo with affected flags set.
 * *num_segments_out receives the total segment count.
 */
static TpVacuumSegmentInfo *
tp_vacuum_identify_affected(
		Relation					  index,
		const TpSegmentGraphSnapshot *segment_snapshot,
		IndexBulkDeleteCallback		  callback,
		void						 *callback_state,
		int							 *num_segments_out,
		int64						 *total_dead_out)
{
	TpVacuumSegmentInfo *segments;
	/* Keep both CTID scratch arrays within roughly one PostgreSQL block. */
	const uint32  ctid_batch_capacity = BLCKSZ / (sizeof(BlockNumber) +
												  sizeof(OffsetNumber));
	BlockNumber	 *ctid_pages;
	OffsetNumber *ctid_offsets;
	int			  capacity				= 32;
	int			  count					= 0;
	int64		  total_dead			= 0;
	bool		  identification_paused = false;

	segments	 = palloc(capacity * sizeof(TpVacuumSegmentInfo));
	ctid_pages	 = palloc(ctid_batch_capacity * sizeof(BlockNumber));
	ctid_offsets = palloc(ctid_batch_capacity * sizeof(OffsetNumber));

	for (int level = 0; level < TP_MAX_LEVELS; level++)
	{
		const BlockNumber *roots;
		uint32			   root_count;

		roots = tp_segment_graph_snapshot_level(
				segment_snapshot, level, &root_count);
		for (uint32 root_idx = 0; root_idx < root_count; root_idx++)
		{
			TpSegmentReader *reader;
			uint32			 seg_dead = 0;
			BlockNumber		 seg	  = roots[root_idx];

			reader = tp_segment_open_ex(index, seg, false);
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

				for (uint32 batch_start = 0;
					 batch_start < reader->header->num_docs;
					 batch_start += ctid_batch_capacity)
				{
					uint32 batch_count =
							Min(ctid_batch_capacity,
								reader->header->num_docs - batch_start);

					tp_segment_read(
							reader,
							reader->header->ctid_pages_offset +
									(uint64)batch_start * sizeof(BlockNumber),
							ctid_pages,
							batch_count * sizeof(BlockNumber));
					tp_segment_read(
							reader,
							reader->header->ctid_offsets_offset +
									(uint64)batch_start * sizeof(OffsetNumber),
							ctid_offsets,
							batch_count * sizeof(OffsetNumber));

					for (uint32 batch_pos = 0; batch_pos < batch_count;
						 batch_pos++)
					{
						ItemPointerData ctid;
						uint32			i = batch_start + batch_pos;

						/*
						 * Skip docs already marked dead in the alive
						 * bitset.  Without this, CTID reuse after a
						 * previous VACUUM would double-count dead docs
						 * and corrupt total_docs in the metapage.
						 */
						if (has_bitset && !tp_segment_is_alive(reader, i))
							continue;

						ItemPointerSet(
								&ctid,
								ctid_pages[batch_pos],
								ctid_offsets[batch_pos]);
						if (ItemPointerIsValid(&ctid) &&
							callback(&ctid, callback_state))
						{
							if (seg_dead >= dead_cap)
							{
								dead_cap = (dead_cap == 0) ? 64 : dead_cap * 2;
								dead_ids =
										dead_ids
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

					if (!identification_paused)
					{
						tp_debug_vacuum_pause_during_identification(index);
						identification_paused = true;
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
				segments[count].level		 = level;
				segments[count].num_docs	 = reader->header->num_docs;
				segments[count].total_tokens = reader->header->total_tokens;
				segments[count].dead_doc_ids = dead_ids;
				segments[count].dead_count	 = seg_dead;
				segments[count].affected	 = (seg_dead > 0);
				segments[count].is_v5 =
						(reader->header->alive_bitset_offset > 0);
				segments[count].already_empty = segments[count].is_v5 &&
												reader->header->alive_count ==
														0;
			}

			total_dead += seg_dead;
			count++;

			tp_segment_close(reader);
		}
	}

	pfree(ctid_offsets);
	pfree(ctid_pages);

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
		char		  **terms;
		int32		   *frequencies;
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

		doc_length = tp_tokenize_text(
				document_text,
				text_config_oid,
				&terms,
				&frequencies,
				&term_count);

		MemoryContextSwitchTo(old_ctx);

		tp_build_context_add_document(
				build_ctx, terms, frequencies, term_count, doc_length, &ctid);
		docs_added++;
		len_added += doc_length;

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

static bool
tp_vacuum_graph_matches(
		Relation index, Page page, const TpSegmentGraphSnapshot *snapshot)
{
	TpIndexMetaPage current;
	bool			matches;

	current = tp_metapage_copy_from_page(index, page);
	matches = current->magic == snapshot->metapage.magic &&
			  current->version == snapshot->metapage.version &&
			  current->text_config_oid == snapshot->metapage.text_config_oid &&
			  current->_unused_total_terms ==
					  snapshot->metapage._unused_total_terms &&
			  current->k1 == snapshot->metapage.k1 &&
			  current->b == snapshot->metapage.b &&
			  current->root_blkno == snapshot->metapage.root_blkno &&
			  current->term_stats_root == snapshot->metapage.term_stats_root &&
			  current->_unused_docid_page ==
					  snapshot->metapage._unused_docid_page &&
			  current->capabilities == snapshot->metapage.capabilities &&
			  memcmp(current->level_heads,
					 snapshot->metapage.level_heads,
					 sizeof(current->level_heads)) == 0 &&
			  memcmp(current->level_counts,
					 snapshot->metapage.level_counts,
					 sizeof(current->level_counts)) == 0;
	pfree(current);
	return matches;
}

static TpSegmentGraphSnapshot *
tp_vacuum_prepare_replacement(
		TpLocalIndexState *index_state,
		Relation		   index,
		uint32			   level,
		BlockNumber		   old_root,
		BlockNumber		  *prev_root,
		BlockNumber		  *next_root)
{
	volatile bool			locked = false;
	TpSegmentGraphSnapshot *snapshot;
	const BlockNumber	   *roots;
	uint32					root_count;
	bool					found = false;

	PG_TRY();
	{
		tp_acquire_index_lock(index_state, LW_SHARED);
		locked	 = true;
		snapshot = tp_segment_graph_snapshot_create(index);
		tp_release_index_lock(index_state);
		locked = false;
	}
	PG_FINALLY();
	{
		if (locked && index_state->lock_held)
			tp_release_index_lock(index_state);
	}
	PG_END_TRY();

	roots = tp_segment_graph_snapshot_level(snapshot, level, &root_count);
	for (uint32 i = 0; i < root_count; i++)
	{
		if (roots[i] != old_root)
			continue;

		*prev_root = (i == 0) ? InvalidBlockNumber : roots[i - 1];
		*next_root = (i + 1 == root_count) ? InvalidBlockNumber : roots[i + 1];
		found	   = true;
		break;
	}

	if (!found)
	{
		tp_segment_graph_snapshot_free(snapshot);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("VACUUM replacement source %u is no longer "
						"published at level %u",
						old_root,
						level)));
	}

	return snapshot;
}

/*
 * Publish one prepared legacy replacement.  Output construction, source-page
 * collection, and tombstone construction happen without the per-index lock.
 * A shared graph snapshot records any concurrent L0 prefix; the exclusive
 * section validates that exact graph and only then swaps the source.
 */
static void
tp_vacuum_replace_segment(
		TpLocalIndexState *index_state,
		Relation		   index,
		uint32			   level,
		BlockNumber		   old_root,
		BlockNumber		   new_root,
		uint64			   docs_shrinkage,
		uint64			   tokens_shrinkage)
{
	BlockNumber		 *old_pages = NULL;
	uint32			  old_page_count;
	FullTransactionId vacuum_fxid;
	volatile bool	  published							  = false;
	volatile bool	  index_locked						  = false;
	volatile Buffer	  metabuf							  = InvalidBuffer;
	volatile Buffer	  new_buf							  = InvalidBuffer;
	volatile Buffer	  prev_buf							  = InvalidBuffer;
	volatile Buffer	  tail_buf							  = InvalidBuffer;
	GenericXLogState *volatile xlog_state				  = NULL;
	TpSegmentGraphSnapshot *volatile publication_snapshot = NULL;
	volatile TpDetachedTombstoneBatch tombstones;

	memset((TpDetachedTombstoneBatch *)&tombstones, 0, sizeof(tombstones));
	tombstones.head = InvalidBlockNumber;
	tombstones.tail = InvalidBlockNumber;

	PG_TRY();
	{
		old_page_count = tp_segment_collect_pages(index, old_root, &old_pages);
		if (old_page_count == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("VACUUM replacement source %u has no pages",
							old_root)));
		vacuum_fxid = GetCurrentFullTransactionId();
		tp_tombstone_build_detached(
				index,
				old_pages,
				old_page_count,
				vacuum_fxid,
				(TpDetachedTombstoneBatch *)&tombstones);

		for (;;)
		{
			BlockNumber		prev_root;
			BlockNumber		next_root;
			Page			current_page;
			TpIndexMetaPage current_meta;
			BlockNumber		current_pending;
			Page			meta_copy;
			TpIndexMetaPage meta;

			publication_snapshot = tp_vacuum_prepare_replacement(
					index_state,
					index,
					level,
					old_root,
					&prev_root,
					&next_root);

			tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
			index_locked = true;
			metabuf		 = ReadBuffer(index, TP_METAPAGE_BLKNO);
			LockBuffer((Buffer)metabuf, BUFFER_LOCK_EXCLUSIVE);
			current_page = BufferGetPage((Buffer)metabuf);
			if (!tp_vacuum_graph_matches(
						index,
						current_page,
						(const TpSegmentGraphSnapshot *)publication_snapshot))
			{
				UnlockReleaseBuffer((Buffer)metabuf);
				metabuf = InvalidBuffer;
				tp_release_index_lock(index_state);
				index_locked = false;
				tp_segment_graph_snapshot_free(
						(TpSegmentGraphSnapshot *)publication_snapshot);
				publication_snapshot = NULL;
				CHECK_FOR_INTERRUPTS();
				continue;
			}

			current_meta	= (TpIndexMetaPage)PageGetContents(current_page);
			current_pending = current_meta->version < TP_METAPAGE_VERSION_V8
									? InvalidBlockNumber
									: current_meta->pending_free_head;

			if (BlockNumberIsValid(prev_root))
			{
				Page		prev_page;
				char	   *prev_content;
				uint32		prev_version;
				BlockNumber recorded_next;

				prev_buf = ReadBuffer(index, prev_root);
				LockBuffer((Buffer)prev_buf, BUFFER_LOCK_EXCLUSIVE);
				prev_page	  = BufferGetPage((Buffer)prev_buf);
				prev_content  = PageGetContents(prev_page);
				prev_version  = ((TpSegmentHeader *)prev_content)->version;
				recorded_next = prev_version <= TP_SEGMENT_FORMAT_VERSION_3
									  ? ((TpSegmentHeaderV3 *)prev_content)
												->next_segment
									  : ((TpSegmentHeader *)prev_content)
												->next_segment;
				if (recorded_next != old_root)
					ereport(ERROR,
							(errcode(ERRCODE_INTERNAL_ERROR),
							 errmsg("VACUUM replacement predecessor "
									"changed before publication")));
			}

			if (BlockNumberIsValid(new_root))
			{
				new_buf = ReadBuffer(index, new_root);
				LockBuffer((Buffer)new_buf, BUFFER_LOCK_EXCLUSIVE);
			}

			xlog_state = GenericXLogStart(index);
			meta_copy  = GenericXLogRegisterBuffer(
					 (GenericXLogState *)xlog_state, (Buffer)metabuf, 0);

			if (BufferIsValid((Buffer)new_buf))
			{
				Page			 new_page;
				TpSegmentHeader *new_header;

				new_page = GenericXLogRegisterBuffer(
						(GenericXLogState *)xlog_state, (Buffer)new_buf, 0);
				((PageHeader)new_page)->pd_lower = BLCKSZ;
				new_header = (TpSegmentHeader *)PageGetContents(new_page);
				new_header->next_segment = next_root;
				new_header->level		 = level;
			}

			if (BufferIsValid((Buffer)prev_buf))
			{
				Page		prev_copy;
				char	   *prev_content;
				uint32		prev_version;
				BlockNumber replacement_next = BlockNumberIsValid(new_root)
													 ? new_root
													 : next_root;

				prev_copy = GenericXLogRegisterBuffer(
						(GenericXLogState *)xlog_state, (Buffer)prev_buf, 0);
				((PageHeader)prev_copy)->pd_lower = BLCKSZ;
				prev_content					  = PageGetContents(prev_copy);
				prev_version = ((TpSegmentHeader *)prev_content)->version;
				if (prev_version <= TP_SEGMENT_FORMAT_VERSION_3)
					((TpSegmentHeaderV3 *)prev_content)->next_segment =
							replacement_next;
				else
					((TpSegmentHeader *)prev_content)->next_segment =
							replacement_next;
			}

			tail_buf = tp_tombstone_attach_detached(
					(GenericXLogState *)xlog_state,
					index,
					*(TpDetachedTombstoneBatch *)&tombstones,
					current_pending);

			tp_metapage_upgrade_to_current(index, meta_copy);
			meta = (TpIndexMetaPage)PageGetContents(meta_copy);
			if (!BlockNumberIsValid(prev_root))
				meta->level_heads[level] = BlockNumberIsValid(new_root)
												 ? new_root
												 : next_root;
			if (!BlockNumberIsValid(new_root))
				meta->level_counts[level]--;
			if (tombstones.container_pages > 0)
				meta->pending_free_head = tombstones.head;
			meta->total_docs = meta->total_docs >= docs_shrinkage
									 ? meta->total_docs - docs_shrinkage
									 : 0;
			meta->total_len	 = meta->total_len >= tokens_shrinkage
									 ? meta->total_len - tokens_shrinkage
									 : 0;

			GenericXLogFinish((GenericXLogState *)xlog_state);
			xlog_state = NULL;
			published  = true;

			if (BufferIsValid((Buffer)tail_buf))
			{
				UnlockReleaseBuffer((Buffer)tail_buf);
				tail_buf = InvalidBuffer;
			}
			if (BufferIsValid((Buffer)prev_buf))
			{
				UnlockReleaseBuffer((Buffer)prev_buf);
				prev_buf = InvalidBuffer;
			}
			if (BufferIsValid((Buffer)new_buf))
			{
				UnlockReleaseBuffer((Buffer)new_buf);
				new_buf = InvalidBuffer;
			}
			UnlockReleaseBuffer((Buffer)metabuf);
			metabuf = InvalidBuffer;
			tp_release_index_lock(index_state);
			index_locked = false;
			tp_segment_graph_snapshot_free(
					(TpSegmentGraphSnapshot *)publication_snapshot);
			publication_snapshot = NULL;
			break;
		}
	}
	PG_CATCH();
	{
		if (!published)
		{
			if (xlog_state != NULL)
				GenericXLogAbort((GenericXLogState *)xlog_state);
			if ((BufferIsValid((Buffer)tail_buf) ||
				 BufferIsValid((Buffer)prev_buf) ||
				 BufferIsValid((Buffer)new_buf) ||
				 BufferIsValid((Buffer)metabuf)) &&
				InterruptHoldoffCount == 0)
				HOLD_INTERRUPTS();
			if (BufferIsValid((Buffer)tail_buf))
				UnlockReleaseBuffer((Buffer)tail_buf);
			if (BufferIsValid((Buffer)prev_buf))
				UnlockReleaseBuffer((Buffer)prev_buf);
			if (BufferIsValid((Buffer)new_buf))
				UnlockReleaseBuffer((Buffer)new_buf);
			if (BufferIsValid((Buffer)metabuf))
				UnlockReleaseBuffer((Buffer)metabuf);
			if (index_locked && index_state->lock_held)
				tp_release_index_lock(index_state);
			if (publication_snapshot != NULL)
				tp_segment_graph_snapshot_free(
						(TpSegmentGraphSnapshot *)publication_snapshot);
			if (BlockNumberIsValid(new_root))
				tp_discard_unpublished_segment(index, new_root);
			tp_tombstone_discard_detached(
					index, *(TpDetachedTombstoneBatch *)&tombstones);
		}
		if (old_pages != NULL)
			pfree(old_pages);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (old_pages != NULL)
		pfree(old_pages);
	if (tombstones.owned_pages != NULL)
		pfree(tombstones.owned_pages);
}

/*
 * Apply dead doc marks to a V5 segment's alive bitset.
 *
 * Loads the bitset, marks dead docs, and writes it back via GenericXLog.
 * Empty segments stay published until serial cleanup compacts their prefix.
 */
static void
tp_vacuum_mark_dead(
		Relation	index,
		BlockNumber root_block,
		uint32	   *dead_doc_ids,
		uint32		dead_count)
{
	TpSegmentReader *reader;
	TpAliveBitset	*bitset;

	reader = tp_segment_open_ex(index, root_block, false);
	if (!reader || !reader->header)
	{
		if (reader)
			tp_segment_close(reader);
		return;
	}

	bitset = tp_alive_bitset_load(reader);
	if (!bitset)
	{
		tp_segment_close(reader);
		return;
	}

	for (uint32 i = 0; i < dead_count; i++)
		tp_alive_bitset_mark_dead(bitset, dead_doc_ids[i]);

	tp_alive_bitset_write(bitset, reader, index);

	tp_alive_bitset_free(bitset);
	tp_segment_close(reader);
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
	TpIndexMetaPage			metap;
	TpSegmentGraphSnapshot *segment_snapshot;
	TpLocalIndexState	   *index_state;
	TpVacuumSegmentInfo	   *segments;
	int						num_segments;
	int64					total_dead;
	volatile bool			maintenance_locked = false;
	volatile bool			index_lock_held	   = false;
	bool parallel_context	 = IsInParallelMode() || IsParallelWorker();
	bool needs_empty_cleanup = false;

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
	if (index_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for \"%s\"",
						RelationGetRelationName(info->index))));
	tp_spill_memtable_if_needed(info->index, index_state, 1);

	/*
	 * Serialize source-derived maintenance before taking the per-index
	 * lock.  Compaction releases its LW_SHARED selection lock while it
	 * derives replacement output, so this heavyweight lock is what keeps
	 * VACUUM from changing selected alive bits or replacing a source until
	 * compaction publishes.
	 *
	 * Acquire after Phase 1's spill, which has its own exclusive per-index
	 * section.  LW_SHARED is held only while copying the published graph.
	 * The maintenance lock keeps those source segments immutable during the
	 * callback walk, bitmap updates, legacy rebuilds, and page collection,
	 * while concurrent spills may safely prepend an L0 prefix.
	 */
	tp_compaction_lock(info->index);
	maintenance_locked = true;
	PG_TRY();
	{
		if (index_state != NULL)
		{
			tp_acquire_index_lock(index_state, LW_SHARED);
			index_lock_held = true;
		}

		/* Snapshot the published roots after spill and maintenance admission.
		 */
		pfree(metap);
		segment_snapshot = tp_segment_graph_snapshot_create(info->index);
		metap			 = &segment_snapshot->metapage;
		if (index_lock_held)
		{
			tp_release_index_lock(index_state);
			index_lock_held = false;
		}

		/* Phase 2: Identify affected segments. */
		segments = tp_vacuum_identify_affected(
				info->index,
				segment_snapshot,
				callback,
				callback_state,
				&num_segments,
				&total_dead);

		for (int i = 0; i < num_segments; i++)
		{
			if (segments[i].already_empty)
			{
				needs_empty_cleanup = true;
				break;
			}
		}

		if (total_dead == 0 && !needs_empty_cleanup)
		{
			stats->num_pages		= 1;
			stats->num_index_tuples = (double)metap->total_docs;
			stats->tuples_removed	= 0;
			stats->pages_deleted	= 0;
			pfree(segments);
			tp_segment_graph_snapshot_free(segment_snapshot);
			goto bulkdelete_done;
		}

		tp_segment_graph_snapshot_free(segment_snapshot);

		elog(DEBUG1,
			 "Tapir VACUUM: %lld dead tuples across %d segments",
			 (long long)total_dead,
			 num_segments);

		/*
		 * Legacy segments have no alive bitmap, so VACUUM must replace them
		 * to remove dead TIDs.  Safe replacement needs an assigned XID to pin
		 * the deferred-reclaim horizon through publication, but PostgreSQL
		 * forbids XID assignment after entering parallel mode.  Fail before
		 * mutating any segment so heap cleanup cannot outpace this index.
		 */
		if (parallel_context)
		{
			for (int i = 0; i < num_segments; i++)
			{
				if (segments[i].affected && !segments[i].is_v5)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot vacuum legacy pg_textsearch "
									"segments during a parallel operation"),
							 errdetail(
									 "Index \"%s\" requires segment "
									 "replacement to remove dead tuples.",
									 RelationGetRelationName(info->index)),
							 errhint("Retry with VACUUM (PARALLEL 0), or "
									 "REINDEX the pg_textsearch index.")));
			}
		}

		/*
		 * Phase 3: mark dead docs or rebuild affected legacy segments.
		 * V5 mutations need only their segment-buffer locks.  Legacy
		 * replacements prepare their output and page list unlocked, then
		 * validate and publish in a brief exclusive section.
		 */
		for (int level = 0; level < TP_MAX_LEVELS; level++)
		{
			for (int i = 0; i < num_segments; i++)
			{
				if ((int)segments[i].level != level || !segments[i].affected)
					continue;

				if (segments[i].is_v5)
				{
					/*
					 * Persist an all-zero bitmap in both serial and
					 * parallel VACUUM.  Serial amvacuumcleanup removes
					 * one empty-bearing prefix through the normal
					 * prepared compaction publication path.
					 */
					(void)tp_vacuum_mark_dead(
							info->index,
							segments[i].root_block,
							segments[i].dead_doc_ids,
							segments[i].dead_count);
				}
				else
				{
					BlockNumber new_root;
					uint64		new_docs		 = 0;
					uint64		new_tokens		 = 0;
					uint64		docs_shrinkage	 = 0;
					uint64		tokens_shrinkage = 0;

					new_root = tp_vacuum_rebuild_segment(
							info->index,
							info->heaprel,
							segments[i].root_block,
							level,
							callback,
							callback_state,
							&new_docs,
							&new_tokens);

					if (segments[i].num_docs > new_docs)
						docs_shrinkage = segments[i].num_docs - new_docs;
					if (segments[i].total_tokens > new_tokens)
						tokens_shrinkage = segments[i].total_tokens -
										   new_tokens;

					tp_vacuum_replace_segment(
							index_state,
							info->index,
							level,
							segments[i].root_block,
							new_root,
							docs_shrinkage,
							tokens_shrinkage);
				}
			}
		}

		/*
		 * tp_vacuumcleanup will set num_index_tuples to the actual live
		 * count; only tuples_removed needs to carry through from here.
		 */
		stats->num_pages	  = 1;
		stats->tuples_removed = (double)total_dead;
		stats->pages_deleted  = 0;

		for (int i = 0; i < num_segments; i++)
		{
			if (segments[i].dead_doc_ids)
				pfree(segments[i].dead_doc_ids);
		}
		pfree(segments);

	bulkdelete_done:
		if (index_lock_held)
		{
			tp_release_index_lock(index_state);
			index_lock_held = false;
		}
	}
	PG_FINALLY();
	{
		if (index_lock_held && index_state != NULL && index_state->lock_held)
			tp_release_index_lock(index_state);
		if (maintenance_locked)
		{
			tp_compaction_unlock(info->index);
			maintenance_locked = false;
		}
	}
	PG_END_TRY();

	return stats;
}

/*
 * Vacuum/cleanup the BM25 index
 */
IndexBulkDeleteResult *
tp_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	TpSegmentGraphSnapshot *segment_snapshot;
	TpLocalIndexState	   *index_state;
	int						freed_pages;
	volatile bool			maintenance_locked = false;
	volatile bool			index_lock_held	   = false;

	/* Initialize stats if not provided */
	if (stats == NULL)
		stats = (IndexBulkDeleteResult *)palloc0(
				sizeof(IndexBulkDeleteResult));

	/*
	 * Spill the memtable so the on-disk memtable chain doesn't
	 * accumulate forever on insert-only tables.  Insert-only
	 * tables skip ambulkdelete (no dead tuples), so without this
	 * call nothing would spill and the chain would keep growing.
	 * Skip under TP_MIN_SPILL_PAGES — that few docs is cheaper
	 * to read from the chain on every query than to compact a
	 * runt L0 segment away.
	 */
	index_state = tp_get_local_index_state(RelationGetRelid(info->index));
	if (index_state != NULL)
		tp_spill_memtable_if_needed(
				info->index, index_state, TP_MIN_SPILL_PAGES);

	/*
	 * Maintenance stabilizes segment payloads while cleanup removes at most
	 * one empty-bearing prefix and counts live documents.  Parallel VACUUM
	 * cannot assign the reclaim XID needed by physical cleanup, so it leaves
	 * zeroed segments for a later serial pass.
	 */
	tp_compaction_lock(info->index);
	maintenance_locked = true;
	PG_TRY();
	{
		if (index_state != NULL && !IsInParallelMode() && !IsParallelWorker())
			(void)tp_compact_empty_step(index_state, info->index);

		if (index_state != NULL)
		{
			tp_acquire_index_lock(index_state, LW_SHARED);
			index_lock_held = true;
		}

		segment_snapshot = tp_segment_graph_snapshot_create(info->index);
		if (index_lock_held)
		{
			tp_release_index_lock(index_state);
			index_lock_held = false;
		}

		stats->num_pages = 1;
		/*
		 * reltuples tracks live docs, which can be less than
		 * metap->total_docs because V5 bitset flips with survivors
		 * reduce alive_count without changing segment.num_docs.  Sum
		 * alive_count across segments for an accurate live count
		 * regardless of whether tp_bulkdelete ran or this is a
		 * no-deletes maintenance round.
		 *
		 * The maintenance lock keeps compaction and another VACUUM from
		 * replacing snapshot sources.  A concurrent spill may prepend L0
		 * but cannot invalidate the copied roots, so the per-index lock is
		 * not held across this walk.
		 */
		stats->num_index_tuples = (double)
				tp_count_live_docs(info->index, segment_snapshot);
		tp_segment_graph_snapshot_free(segment_snapshot);
		tp_compaction_unlock(info->index);
		maintenance_locked = false;

		/*
		 * Return DEAD memtable orphan blocks to the index FSM once
		 * their dead_fxid is older than the global visibility horizon.
		 * LW_SHARED is enough: only already-unlinked DEAD pages are
		 * touched; live inserts/scans hold the same lock and mutate
		 * the metapage chain, not these blocks.  Spill (LW_EXCLUSIVE)
		 * waits until this scan finishes.
		 */
		if (index_state != NULL)
		{
			tp_acquire_index_lock(index_state, LW_SHARED);
			index_lock_held = true;
		}
		freed_pages =
				tp_reclaim_dead_memtable_pages(info->index, info->heaprel);
		if (index_lock_held)
		{
			tp_release_index_lock(index_state);
			index_lock_held = false;
		}

		if (stats->pages_deleted == 0 && stats->tuples_removed == 0 &&
			freed_pages == 0)
			stats->pages_free = 0;

		if (freed_pages != 0)
		{
			stats->pages_free += freed_pages;
			IndexFreeSpaceMapVacuum(info->index);
		}

		/*
		 * Drain past-horizon displaced segment pages (issue #380).
		 * Unlike the memtable reclaim above, this MUTATES the shared
		 * tombstone chain (metap->pending_free_head and tombstone
		 * next_page links), so it must run under LW_EXCLUSIVE, not
		 * LW_SHARED.  tp_tombstone_drain takes/releases the lock per
		 * drained tombstone (own_lock=true) so concurrent reads never
		 * wait more than a single unlink plus its page frees.
		 */
		if (index_state != NULL)
		{
			uint32 drained = tp_tombstone_drain(
					info->index,
					index_state,
					tp_reclaim_horizon(info->heaprel),
					/* own_lock */ true);

			if (drained != 0)
			{
				stats->pages_free += drained;
				IndexFreeSpaceMapVacuum(info->index);
			}
		}
	}
	PG_FINALLY();
	{
		if (index_lock_held && index_state != NULL && index_state->lock_held)
			tp_release_index_lock(index_state);
		if (maintenance_locked)
			tp_compaction_unlock(info->index);
	}
	PG_END_TRY();

	return stats;
}

/*
 * Scan the index main fork for memtable pages stamped DEAD at spill
 * and recycle blocks whose dead_fxid is older than the visibility
 * horizon for `heaprel` (FullTransactionId compare).  Does not
 * WAL-log page bodies or clear DEAD flags; reuse overwrites via
 * tp_memtable_alloc_page.  Caller holds per-index LW_SHARED (or
 * stronger); excludes spill via LW_EXCLUSIVE incompatibility.
 *
 * Crash-safety guard: we build a set of blocks reachable from the
 * current memtable chain and skip freeing any page in that set.
 * This prevents corruption if a crash between tp_spill_finalize and
 * tp_memtable_mark_chain_dead left pages stamped DEAD but still
 * reachable via metap.head.  See tp_collect_reachable_chain_blocks.
 */
int
tp_reclaim_dead_memtable_pages(Relation indexrel, Relation heaprel)
{
	int				  reclaimed_pages = 0;
	BlockNumber		  nblocks		  = RelationGetNumberOfBlocks(indexrel);
	BlockNumber		  blk;
	TransactionId	  oldest;
	FullTransactionId oldest_fxid;
	HTAB			 *reachable;

	oldest = GetOldestNonRemovableTransactionId(heaprel);
	oldest_fxid =
			tp_full_xid_from_allowable_at(ReadNextFullTransactionId(), oldest);

	/*
	 * Build set of blocks still reachable from metap.head.
	 * Pages in this set must not be freed even if stamped DEAD.
	 */
	reachable = tp_collect_reachable_chain_blocks(indexrel);

	for (blk = TP_METAPAGE_BLKNO + 1; blk < nblocks; blk++)
	{
		Buffer				  buf;
		Page				  page;
		TpMemtablePageHeader *hdr;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(indexrel, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		if (!tp_memtable_page_is_valid(page) ||
			!tp_memtable_page_is_dead(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		hdr = tp_memtable_page_header(page);
		if (FullTransactionIdPrecedes(hdr->dead_fxid, oldest_fxid))
		{
			/* Skip pages still reachable from live chain */
			bool found;
			hash_search(reachable, &blk, HASH_FIND, &found);
			if (!found)
			{
				/*
				 * Release the SHARE lock before returning the page to
				 * the FSM: tp_record_free_index_page re-locks
				 * EXCLUSIVE to write the recyclable free stamp so a
				 * later allocator can tell this deliberately-freed
				 * page from a live one.  The page is DEAD (unlinked)
				 * and unreachable, and is not yet in the FSM, so no
				 * concurrent backend can allocate or resurrect it
				 * between the release and the stamp.
				 */
				UnlockReleaseBuffer(buf);
				tp_record_free_index_page(indexrel, blk);
				reclaimed_pages++;
				continue;
			}
		}

		UnlockReleaseBuffer(buf);
	}

	hash_destroy(reachable);
	return reclaimed_pages;
}
