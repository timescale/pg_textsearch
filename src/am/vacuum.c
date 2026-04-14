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
#include <catalog/namespace.h>
#include <commands/progress.h>
#include <commands/vacuum.h>
#include <fmgr.h>
#include <lib/dshash.h>
#include <storage/bufmgr.h>
#include <storage/indexfsm.h>
#include <tsearch/ts_utils.h>
#include <utils/builtins.h>
#include <utils/fmgrprotos.h>
#include <utils/regproc.h>
#include <utils/rel.h>
#include <utils/snapmgr.h>

#include "am.h"
#include "am/build_context.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "segment/merge.h"
#include "segment/segment.h"
#include "segment/segment_io.h"
#include "state/metapage.h"
#include "state/state.h"

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
	uint32		num_docs;
	int64		dead_count;
	bool		affected;
} TpVacuumSegmentInfo;

/*
 * Spill memtable to L0 segment if non-empty.
 *
 * During VACUUM, we want all index data in segments for uniform
 * processing.  This forces any in-memory data to disk.
 *
 * Caller must hold at least AccessExclusiveLock on the index
 * (standard for ambulkdelete).  The memtable is in DSA, which
 * is pinned for the lifetime of the shared index state and
 * cannot be freed by another backend.  The pre-lock read of
 * total_postings is a fast bailout; if it races with an insert,
 * the worst case is we enter tp_write_segment with an empty
 * memtable, which is a harmless no-op.
 */
static void
tp_vacuum_spill_memtable(Relation index, TpLocalIndexState *index_state)
{
	TpMemtable *memtable;
	BlockNumber segment_root;

	if (!index_state || !index_state->shared)
		return;

	memtable = get_memtable(index_state);
	if (!memtable || pg_atomic_read_u64(&memtable->total_postings) == 0)
		return;

	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

	segment_root = tp_write_segment(index_state, index);
	if (segment_root != InvalidBlockNumber)
	{
		tp_clear_memtable(index_state);
		tp_clear_docid_pages(index);
		tp_link_l0_chain_head(index, segment_root);
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
			int64			 seg_dead = 0;

			reader = tp_segment_open_ex(index, seg, true);
			if (!reader || !reader->header)
			{
				if (reader)
					tp_segment_close(reader);
				break;
			}

			/* Check each CTID against the callback */
			for (uint32 i = 0; i < reader->header->num_docs; i++)
			{
				ItemPointerData ctid;

				tp_segment_lookup_ctid(reader, i, &ctid);
				if (ItemPointerIsValid(&ctid) &&
					callback(&ctid, callback_state))
				{
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
			segments[count].dead_count	 = seg_dead;
			segments[count].affected	 = (seg_dead > 0);

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
	AttrNumber		 attnum;
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

	/* Get the indexed column number */
	attnum = index->rd_index->indkey.values[0];

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
		Datum			text_datum;
		bool			isnull;
		text		   *document_text;
		Datum			tsvector_datum;
		TSVector		tsvector;
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

		/* Extract text from indexed column */
		text_datum =
				heap_getattr(tuple, attnum, RelationGetDescr(heap), &isnull);

		if (isnull)
		{
			ReleaseBuffer(heap_buf);
			continue;
		}

		/* Tokenize in per-doc context (includes detoasting) */
		old_ctx = MemoryContextSwitchTo(per_doc_ctx);

		document_text = DatumGetTextPP(text_datum);

		tsvector_datum = DirectFunctionCall2Coll(
				to_tsvector_byid,
				InvalidOid,
				ObjectIdGetDatum(text_config_oid),
				PointerGetDatum(document_text));
		tsvector = DatumGetTSVector(tsvector_datum);

		doc_length = tp_extract_terms_from_tsvector(
				tsvector, &terms, &frequencies, &term_count);

		MemoryContextSwitchTo(old_ctx);

		if (term_count > 0)
		{
			tp_build_context_add_document(
					build_ctx,
					terms,
					frequencies,
					term_count,
					doc_length,
					&ctid);
			docs_added++;
			len_added += doc_length;
		}

		MemoryContextReset(per_doc_ctx);
		ReleaseBuffer(heap_buf);
	}

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
			/* Update prev's next_segment */
			Page			 prev_page;
			TpSegmentHeader *prev_header;

			prev_buf = ReadBuffer(index, prev_root);
			LockBuffer(prev_buf, BUFFER_LOCK_EXCLUSIVE);
			prev_page = GenericXLogRegisterBuffer(xlog_state, prev_buf, 0);
			/* Ensure pd_lower covers content for GenericXLog */
			((PageHeader)prev_page)->pd_lower = BLCKSZ;
			prev_header = (TpSegmentHeader *)PageGetContents(prev_page);

			if (new_root != InvalidBlockNumber)
				prev_header->next_segment = new_root;
			else
			{
				prev_header->next_segment = old_next;
				meta_ptr->level_counts[level]--;
			}
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
 * Bulk delete callback for vacuum and CREATE INDEX CONCURRENTLY
 *
 * Four-phase approach:
 * 1. Spill memtable to segments (all data in uniform format)
 * 2. Identify segments containing dead CTIDs (O(segments) memory)
 * 3. Rebuild affected segments from heap via TpBuildContext
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

	/* Phase 1: Spill memtable so all data is in segments */
	index_state = tp_get_local_index_state(RelationGetRelid(info->index));
	if (index_state != NULL)
		tp_vacuum_spill_memtable(info->index, index_state);

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
		 "Tapir VACUUM: %lld dead tuples across %d segments, "
		 "rebuilding affected segments",
		 (long long)total_dead,
		 num_segments);

	/* Phase 3: Rebuild affected segments */
	{
		uint64 new_total_docs = 0;

		/*
		 * Walk segments per-level to track prev pointers for
		 * chain replacement.
		 */
		for (int level = 0; level < TP_MAX_LEVELS; level++)
		{
			BlockNumber prev = InvalidBlockNumber;

			for (int i = 0; i < num_segments; i++)
			{
				if ((int)segments[i].level != level)
					continue;

				if (segments[i].affected)
				{
					BlockNumber new_root;
					uint64		seg_docs;

					new_root = tp_vacuum_rebuild_segment(
							info->index,
							info->heaprel,
							segments[i].root_block,
							level,
							callback,
							callback_state,
							&seg_docs,
							NULL);

					tp_vacuum_replace_segment(
							info->index,
							level,
							segments[i].root_block,
							new_root,
							prev);

					new_total_docs += seg_docs;

					/*
					 * If we replaced (not removed), the new
					 * segment becomes prev for the next
					 * iteration.
					 */
					if (new_root != InvalidBlockNumber)
						prev = new_root;
					/* else prev stays the same */
				}
				else
				{
					new_total_docs += segments[i].num_docs;
					prev = segments[i].root_block;
				}
			}
		}

		/* Phase 4: Update metapage statistics */
		{
			Buffer			  mbuf;
			GenericXLogState *xlog_state;
			Page			  mpage;
			TpIndexMetaPage	  mp;

			mbuf = ReadBuffer(info->index, TP_METAPAGE_BLKNO);
			LockBuffer(mbuf, BUFFER_LOCK_EXCLUSIVE);

			xlog_state = GenericXLogStart(info->index);
			mpage	   = GenericXLogRegisterBuffer(xlog_state, mbuf, 0);
			mp		   = (TpIndexMetaPage)PageGetContents(mpage);

			if (mp->total_docs >= (uint64)total_dead)
				mp->total_docs -= total_dead;
			else
				mp->total_docs = new_total_docs;

			GenericXLogFinish(xlog_state);
			UnlockReleaseBuffer(mbuf);
		}
	}

	/* Fill in return stats */
	{
		TpIndexMetaPage mp = tp_get_metapage(info->index);

		if (mp)
		{
			stats->num_pages		= 1;
			stats->num_index_tuples = (double)mp->total_docs;
			stats->tuples_removed	= (double)total_dead;
			stats->pages_deleted	= 0;
			pfree(mp);
		}
	}

	pfree(metap);
	pfree(segments);

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
		stats->num_pages		= 1;
		stats->num_index_tuples = (double)metap->total_docs;

		/* Report current usage statistics */
		if (stats->pages_deleted == 0 && stats->tuples_removed == 0)
		{
			stats->pages_free = 0;
		}

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
