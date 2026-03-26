/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.c - Parallel index build with leader-only merge
 *
 * Architecture:
 * - Phase 1: Each worker scans a heap partition, builds a local
 *   TpBuildContext, flushes L0 segments to BufFile. Workers
 *   report segment info (offsets and sizes).
 * - Barrier: Leader opens all worker BufFiles, performs a single
 *   N-way merge of ALL segments directly to paged storage.
 * - Leader links the single merged segment, updates metapage,
 *   then wakes workers to exit.
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <commands/progress.h>
#include <miscadmin.h>
#include <storage/buffile.h>
#include <storage/bufmgr.h>
#include <storage/condition_variable.h>
#include <tsearch/ts_type.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/wait_event.h>

#include "am.h"
#include "build_context.h"
#include "build_parallel.h"
#include "constants.h"
#include "segment/merge.h"
#include "segment/merge_internal.h"
#include "segment/pagemapper.h"
#include "segment/segment.h"
#include "segment/segment_io.h"
#include "state/metapage.h"

/* ----------------------------------------------------------------
 * Worker-local segment tracking
 * ----------------------------------------------------------------
 */

typedef struct WorkerSegmentEntry
{
	uint64 offset;	  /* BufFile offset */
	uint64 data_size; /* Segment bytes */
} WorkerSegmentEntry;

typedef struct WorkerSegmentTracker
{
	WorkerSegmentEntry *entries;
	uint32				count;
	uint32				capacity;
	uint64				buffile_end; /* logical end of BufFile */
} WorkerSegmentTracker;

static void
tracker_init(WorkerSegmentTracker *tracker)
{
	tracker->capacity	 = 32;
	tracker->count		 = 0;
	tracker->buffile_end = 0;
	tracker->entries = palloc(tracker->capacity * sizeof(WorkerSegmentEntry));
}

static void
tracker_add_segment(
		WorkerSegmentTracker *tracker, uint64 offset, uint64 data_size)
{
	if (tracker->count >= tracker->capacity)
	{
		tracker->capacity *= 2;
		tracker->entries = repalloc(
				tracker->entries,
				tracker->capacity * sizeof(WorkerSegmentEntry));
	}
	tracker->entries[tracker->count].offset	   = offset;
	tracker->entries[tracker->count].data_size = data_size;
	tracker->count++;
}

static void
tracker_destroy(WorkerSegmentTracker *tracker)
{
	if (tracker->entries)
		pfree(tracker->entries);
}

/* ----------------------------------------------------------------
 * Shared memory estimation and initialization
 * ----------------------------------------------------------------
 */

Size
tp_parallel_build_estimate_shmem(
		Relation heap, Snapshot snapshot, int nworkers)
{
	Size size;

	(void)heap;
	(void)snapshot;

	/* Base shared structure (includes per-worker block ranges) */
	size = MAXALIGN(sizeof(TpParallelBuildShared));

	/* Per-worker result array */
	size = add_size(size, MAXALIGN(sizeof(TpParallelWorkerResult) * nworkers));

	return size;
}

static void
tp_init_parallel_shared(
		TpParallelBuildShared *shared,
		Relation			   heap,
		Relation			   index,
		Oid					   text_config_oid,
		AttrNumber			   attnum,
		double				   k1,
		double				   b,
		int					   nworkers)
{
	TpParallelWorkerResult *results;
	int						i;

	memset(shared, 0, sizeof(TpParallelBuildShared));

	/* Immutable configuration */
	shared->heaprelid		= RelationGetRelid(heap);
	shared->indexrelid		= RelationGetRelid(index);
	shared->text_config_oid = text_config_oid;
	shared->attnum			= attnum;
	shared->k1				= k1;
	shared->b				= b;
	shared->nworkers		= nworkers;

	/* Coordination */
	ConditionVariableInit(&shared->all_done_cv);
	pg_atomic_init_u32(&shared->workers_done, 0);
	pg_atomic_init_u64(&shared->tuples_done, 0);

	/* TID range scan coordination */
	shared->nworkers_launched = 0;
	pg_atomic_init_u32(&shared->scan_ready, 0);

	/* Phase coordination */
	pg_atomic_init_u32(&shared->phase1_done, 0);
	ConditionVariableInit(&shared->phase2_cv);
	pg_atomic_init_u32(&shared->phase2_ready, 0);

	/* Initialize per-worker results */
	results = TpParallelWorkerResults(shared);
	for (i = 0; i < nworkers; i++)
		memset(&results[i], 0, sizeof(TpParallelWorkerResult));
}

/* ----------------------------------------------------------------
 * Worker entry point
 * ----------------------------------------------------------------
 */
PGDLLEXPORT void
tp_parallel_build_worker_main(dsm_segment *seg, shm_toc *toc)
{
	TpParallelBuildShared  *shared;
	TpParallelWorkerResult *my_result;
	Relation				heap;
	Relation				index;
	TableScanDesc			scan;
	Snapshot				snap;
	TupleTableSlot		   *slot;
	TpBuildContext		   *build_ctx;
	MemoryContext			build_tmpctx;
	MemoryContext			oldctx;
	Size					budget;
	int						worker_id;
	BufFile				   *buffile;
	char					file_name[64];
	WorkerSegmentTracker	tracker;

	/* Attach to shared memory */
	shared = (TpParallelBuildShared *)
			shm_toc_lookup(toc, TP_PARALLEL_KEY_SHARED, false);

	worker_id = ParallelWorkerNumber;
	my_result = &TpParallelWorkerResults(shared)[worker_id];

	/* Open heap and index relations */
	heap  = table_open(shared->heaprelid, AccessShareLock);
	index = index_open(shared->indexrelid, AccessExclusiveLock);

	/* Attach to SharedFileSet and create worker's BufFile */
	SharedFileSetAttach(&shared->fileset, seg);
	snprintf(file_name, sizeof(file_name), "tp_worker_%d", worker_id);
	buffile = BufFileCreateFileSet(&shared->fileset.fs, file_name);

	/*
	 * Wait for leader to set up per-worker block ranges.
	 * This is a brief spin (microseconds) after launch.
	 */
	while (pg_atomic_read_u32(&shared->scan_ready) == 0)
		pg_usleep(100);
	pg_read_barrier();

	/*
	 * Open a TID range scan limited to this worker's block range.
	 * Each worker scans a contiguous, non-overlapping range of
	 * heap blocks, ensuring disjoint CTIDs across workers.
	 */
	snap = GetTransactionSnapshot();
#if PG_VERSION_NUM >= 180000
	snap = RegisterSnapshot(snap);
#endif

	slot = table_slot_create(heap, NULL);
	{
		BlockNumber start_blk = shared->worker_start_block[worker_id];
		BlockNumber end_blk	  = shared->worker_end_block[worker_id];

		if (start_blk < end_blk)
		{
			ItemPointerData min_tid, max_tid;

			ItemPointerSet(&min_tid, start_blk, FirstOffsetNumber);
			ItemPointerSet(&max_tid, end_blk - 1, MaxOffsetNumber);
			scan = table_beginscan_tidrange(heap, snap, &min_tid, &max_tid);
		}
		else
		{
			/* Empty range: no blocks assigned to this worker */
			scan = NULL;
		}
	}

	/*
	 * Per-worker memory budget: split maintenance_work_mem across
	 * workers. Minimum 64MB per worker to avoid excessive flushing.
	 */
	budget = (Size)maintenance_work_mem * 1024L / shared->nworkers_launched;
	if (budget < 64L * 1024 * 1024)
		budget = 64L * 1024 * 1024;

	build_ctx = tp_build_context_create(budget);
	tracker_init(&tracker);

	build_tmpctx = AllocSetContextCreate(
			CurrentMemoryContext,
			"parallel build per-doc temp",
			ALLOCSET_DEFAULT_SIZES);

	/* Process tuples from this worker's block range */
	while (scan != NULL &&
		   table_scan_getnextslot_tidrange(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Datum		text_datum;
		text	   *document_text;
		ItemPointer ctid;
		Datum		tsvector_datum;
		TSVector	tsvector;
		char	  **terms;
		int32	   *frequencies;
		int			term_count;
		int			doc_length;

		text_datum = slot_getattr(slot, shared->attnum, &isnull);
		if (isnull)
			goto next_tuple;

		document_text = DatumGetTextPP(text_datum);
		slot_getallattrs(slot);
		ctid = &slot->tts_tid;

		if (!ItemPointerIsValid(ctid))
			goto next_tuple;

		/* Tokenize in temporary context */
		oldctx = MemoryContextSwitchTo(build_tmpctx);

		tsvector_datum = DirectFunctionCall2Coll(
				to_tsvector_byid,
				InvalidOid,
				ObjectIdGetDatum(shared->text_config_oid),
				PointerGetDatum(document_text));
		tsvector = DatumGetTSVector(tsvector_datum);

		doc_length = tp_extract_terms_from_tsvector(
				tsvector, &terms, &frequencies, &term_count);

		MemoryContextSwitchTo(oldctx);

		if (term_count > 0)
		{
			tp_build_context_add_document(
					build_ctx,
					terms,
					frequencies,
					term_count,
					doc_length,
					ctid);
		}

		/* Reset per-doc context */
		MemoryContextReset(build_tmpctx);

		/* Budget-based flush to BufFile */
		if (tp_build_context_should_flush(build_ctx))
		{
			uint64 data_size;
			uint64 seg_offset;

			my_result->total_docs += build_ctx->num_docs;
			my_result->total_len += build_ctx->total_len;

			/* L0 segment starts at current end of BufFile */
			seg_offset = tracker.buffile_end;
			{
				int	  fileno;
				off_t file_offset;

				tp_buffile_decompose_offset(seg_offset, &fileno, &file_offset);
				BufFileSeek(buffile, fileno, file_offset, SEEK_SET);
			}

			data_size = tp_write_segment_to_buffile(build_ctx, buffile);

			/* Track this L0 segment */
			tracker_add_segment(&tracker, seg_offset, data_size);
			tracker.buffile_end = seg_offset + data_size;

			tp_build_context_reset(build_ctx);
		}

	next_tuple:
		my_result->tuples_scanned++;

		if (my_result->tuples_scanned % TP_PROGRESS_REPORT_INTERVAL == 0)
		{
			pg_atomic_add_fetch_u64(
					&shared->tuples_done, TP_PROGRESS_REPORT_INTERVAL);
			CHECK_FOR_INTERRUPTS();
		}
	}

	/* Flush remaining data */
	if (build_ctx->num_docs > 0)
	{
		uint64 data_size;
		uint64 seg_offset;

		my_result->total_docs += build_ctx->num_docs;
		my_result->total_len += build_ctx->total_len;

		seg_offset = tracker.buffile_end;
		{
			int	  fileno;
			off_t file_offset;

			tp_buffile_decompose_offset(seg_offset, &fileno, &file_offset);
			BufFileSeek(buffile, fileno, file_offset, SEEK_SET);
		}

		data_size = tp_write_segment_to_buffile(build_ctx, buffile);

		tracker_add_segment(&tracker, seg_offset, data_size);
		tracker.buffile_end = seg_offset + data_size;
	}

	/*
	 * Phase 1 complete: report segments to leader.
	 * All segments are L0 (no worker-side compaction).
	 */
	{
		uint32 i;

		for (i = 0; i < tracker.count; i++)
		{
			if (i >= TP_MAX_WORKER_SEGMENTS)
			{
				elog(WARNING,
					 "worker %d has too many segments (%u), "
					 "truncating",
					 worker_id,
					 tracker.count);
				break;
			}

			my_result->seg_offsets[i] = tracker.entries[i].offset;
			my_result->seg_sizes[i]	  = tracker.entries[i].data_size;
		}
		my_result->final_segment_count =
				Min(tracker.count, TP_MAX_WORKER_SEGMENTS);
	}

	/* Export BufFile so leader can reopen */
	BufFileExportFileSet(buffile);
	BufFileClose(buffile);

	/* Cleanup Phase 1 resources */
	tp_build_context_destroy(build_ctx);
	tracker_destroy(&tracker);
	MemoryContextDelete(build_tmpctx);

	if (scan != NULL)
		table_endscan(scan);
#if PG_VERSION_NUM >= 180000
	UnregisterSnapshot(snap);
#endif
	ExecDropSingleTupleTableSlot(slot);

	/* Signal Phase 1 done, wait for leader to finish merge */
	pg_atomic_fetch_add_u32(&shared->phase1_done, 1);
	ConditionVariableBroadcast(&shared->all_done_cv);

	ConditionVariablePrepareToSleep(&shared->phase2_cv);
	while (pg_atomic_read_u32(&shared->phase2_ready) == 0)
		ConditionVariableSleep(&shared->phase2_cv, PG_WAIT_EXTENSION);
	ConditionVariableCancelSleep();

	/* No Phase 2 work — leader did the merge. Just exit. */
	index_close(index, AccessExclusiveLock);
	table_close(heap, AccessShareLock);

	pg_atomic_fetch_add_u32(&shared->workers_done, 1);
	ConditionVariableSignal(&shared->all_done_cv);
}

/* ----------------------------------------------------------------
 * Leader: main parallel build entry point
 * ----------------------------------------------------------------
 */
IndexBuildResult *
tp_build_parallel(
		Relation   heap,
		Relation   index,
		IndexInfo *indexInfo,
		Oid		   text_config_oid,
		double	   k1,
		double	   b,
		int		   nworkers)
{
	IndexBuildResult	  *result;
	ParallelContext		  *pcxt;
	TpParallelBuildShared *shared;
	Snapshot			   snapshot;
	Size				   shmem_size;
	int					   launched;
	uint64				   total_docs = 0;
	uint64				   total_len  = 0;

	/* Ensure reasonable number of workers */
	if (nworkers > TP_MAX_PARALLEL_WORKERS)
		nworkers = TP_MAX_PARALLEL_WORKERS;

	/* Report loading phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);

	/* Report estimated tuple count for progress tracking */
	{
		double reltuples  = heap->rd_rel->reltuples;
		int64  tuples_est = (reltuples > 0) ? (int64)reltuples : 0;

		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_TOTAL, tuples_est);
	}

	/* Get snapshot for parallel scan */
	snapshot = GetTransactionSnapshot();
#if PG_VERSION_NUM >= 180000
	/* PG18: Must register the snapshot for index builds */
	snapshot = RegisterSnapshot(snapshot);
#endif

	/* Calculate shared memory size */
	shmem_size = tp_parallel_build_estimate_shmem(heap, snapshot, nworkers);

	/* Enter parallel mode and create context */
	EnterParallelMode();
	pcxt = CreateParallelContext(
			"pg_textsearch", "tp_parallel_build_worker_main", nworkers);

	/* Estimate and allocate shared memory */
	shm_toc_estimate_chunk(&pcxt->estimator, shmem_size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	InitializeParallelDSM(pcxt);

	/* Allocate and initialize shared state */
	shared = (TpParallelBuildShared *)shm_toc_allocate(pcxt->toc, shmem_size);
	tp_init_parallel_shared(
			shared,
			heap,
			index,
			text_config_oid,
			indexInfo->ii_IndexAttrNumbers[0],
			k1,
			b,
			nworkers);

	/* Initialize SharedFileSet for worker temp files */
	SharedFileSetInit(&shared->fileset, pcxt->seg);

	/* Insert shared state into TOC */
	shm_toc_insert(pcxt->toc, TP_PARALLEL_KEY_SHARED, shared);

	/* Launch workers */
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;

	if (launched == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("parallel index build: could not launch "
						"any workers"),
				 errhint("Increase max_worker_processes or "
						 "reduce "
						 "max_parallel_maintenance_workers.")));

	ereport(NOTICE,
			(errmsg("parallel index build: launched %d of %d "
					"requested workers",
					launched,
					nworkers)));

	/*
	 * Compute per-worker block ranges for disjoint TID scans.
	 * Must be done after launch so we know `launched`. Workers
	 * spin on scan_ready until ranges are set.
	 */
	{
		BlockNumber nblocks			  = RelationGetNumberOfBlocks(heap);
		BlockNumber blocks_per_worker = nblocks / launched;
		BlockNumber remainder		  = nblocks % launched;
		BlockNumber cursor			  = 0;
		int			i;

		for (i = 0; i < launched; i++)
		{
			BlockNumber count			  = blocks_per_worker +
											(i < (int)remainder ? 1 : 0);
			shared->worker_start_block[i] = cursor;
			shared->worker_end_block[i]	  = cursor + count;
			cursor += count;
		}
		shared->nworkers_launched = launched;
		pg_write_barrier();
		pg_atomic_write_u32(&shared->scan_ready, 1);
	}

	/*
	 * Phase 1 wait: wait for all workers to finish BufFile phase.
	 * Workers signal phase1_done and then block on phase2_cv.
	 */
	ConditionVariablePrepareToSleep(&shared->all_done_cv);
	while (pg_atomic_read_u32(&shared->phase1_done) < (uint32)launched)
	{
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_DONE,
				(int64)pg_atomic_read_u64(&shared->tuples_done));

		ConditionVariableTimedSleep(
				&shared->all_done_cv, 1000 /* ms */, PG_WAIT_EXTENSION);
	}
	ConditionVariableCancelSleep();

	/* Collect corpus stats from all workers */
	{
		TpParallelWorkerResult *results;
		int						i;

		results = TpParallelWorkerResults(shared);
		for (i = 0; i < launched; i++)
		{
			total_docs += results[i].total_docs;
			total_len += results[i].total_len;
		}
	}

	/* Report final tuple count */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_TUPLES_DONE, (int64)total_docs);

	/* Report writing phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_WRITING);

	/*
	 * Leader-only merge: open all worker BufFile segments and
	 * perform a single N-way merge directly to paged storage.
	 */
	{
		TpParallelWorkerResult *results = TpParallelWorkerResults(shared);
		BufFile				  **open_files;
		bool				   *file_opened;
		TpSegmentReader		  **readers;
		TpMergeSource		   *sources;
		uint32					total_segments = 0;
		uint32					num_sources	   = 0;
		uint64					total_tokens   = 0;
		int						w;
		uint32					i;
		TpMergedTerm		   *merged_terms	 = NULL;
		uint32					num_merged_terms = 0;
		uint32					merged_capacity	 = 0;
		MemoryContext			merge_ctx;
		MemoryContext			old_ctx;

		/* Count total segments across all workers */
		for (w = 0; w < launched; w++)
			total_segments += results[w].final_segment_count;

		/* Open worker BufFiles and create segment readers */
		open_files	= palloc0(sizeof(BufFile *) * launched);
		file_opened = palloc0(sizeof(bool) * launched);
		readers		= palloc0(sizeof(TpSegmentReader *) * total_segments);
		sources		= palloc0(sizeof(TpMergeSource) * total_segments);

		{
			uint32 reader_idx = 0;

			for (w = 0; w < launched; w++)
			{
				uint32 s;
				char   fname[64];

				if (results[w].final_segment_count == 0)
					continue;

				snprintf(fname, sizeof(fname), "tp_worker_%d", w);
				open_files[w] = BufFileOpenFileSet(
						&shared->fileset.fs, fname, O_RDONLY, false);
				file_opened[w] = true;

				for (s = 0; s < results[w].final_segment_count; s++)
				{
					readers[reader_idx] = tp_segment_open_from_buffile(
							open_files[w], results[w].seg_offsets[s]);
					if (!readers[reader_idx])
					{
						reader_idx++;
						continue;
					}

					if (merge_source_init_from_reader(
								&sources[num_sources], readers[reader_idx]))
					{
						total_tokens +=
								readers[reader_idx]->header->total_tokens;
						/*
						 * Source now owns this reader; clear
						 * slot so cleanup won't double-close.
						 */
						readers[reader_idx] = NULL;
						num_sources++;
					}
					reader_idx++;
				}
			}
		}

		if (num_sources > 0)
		{
			TpMergeSink sink;
			BlockNumber segment_root;

			/* N-way term merge */
			merge_ctx = AllocSetContextCreate(
					CurrentMemoryContext,
					"Leader Merge",
					ALLOCSET_DEFAULT_SIZES);
			old_ctx = MemoryContextSwitchTo(merge_ctx);

			while (true)
			{
				int			  min_idx;
				const char	 *min_term;
				TpMergedTerm *current_merged;

				min_idx = merge_find_min_source(sources, num_sources);
				if (min_idx < 0)
					break;

				min_term = sources[min_idx].current_term;

				if (num_merged_terms >= merged_capacity)
				{
					merged_capacity = merged_capacity == 0
											? 1024
											: merged_capacity * 2;
					if (merged_terms == NULL)
						merged_terms = palloc_extended(
								merged_capacity * sizeof(TpMergedTerm),
								MCXT_ALLOC_HUGE);
					else
						merged_terms = repalloc_huge(
								merged_terms,
								merged_capacity * sizeof(TpMergedTerm));
				}

				current_merged				 = &merged_terms[num_merged_terms];
				current_merged->term_len	 = strlen(min_term);
				current_merged->term		 = pstrdup(min_term);
				current_merged->segment_refs = NULL;
				current_merged->num_segment_refs	  = 0;
				current_merged->segment_refs_capacity = 0;
				current_merged->posting_offset		  = 0;
				current_merged->posting_count		  = 0;
				num_merged_terms++;

				for (i = 0; i < num_sources; i++)
				{
					if (sources[i].exhausted)
						continue;

					if (strcmp(sources[i].current_term,
							   current_merged->term) == 0)
					{
						merged_term_add_segment_ref(
								current_merged, i, &sources[i].current_entry);
						merge_source_advance(&sources[i]);
					}
				}

				CHECK_FOR_INTERRUPTS();
			}

			MemoryContextSwitchTo(old_ctx);

			/* Write single merged segment to index pages */
			merge_sink_init_pages(&sink, index);
			if (sink.writer.pages_allocated == 0)
				elog(ERROR, "merge: failed to allocate segment pages");
			segment_root = sink.writer.pages[0];

			write_merged_segment_to_sink(
					&sink,
					merged_terms,
					num_merged_terms,
					sources,
					num_sources,
					0, /* target_level: L0 */
					total_tokens,
					true /* disjoint_sources */);

			/*
			 * Flush dirty buffers before updating the metapage,
			 * ensuring merged segment data is durable first.
			 */
			FlushRelationBuffers(index);

			/* Link as L0 head in metapage */
			{
				Buffer			metabuf;
				Page			metapage;
				TpIndexMetaPage metap;

				metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
				LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
				metapage = BufferGetPage(metabuf);
				metap	 = (TpIndexMetaPage)PageGetContents(metapage);

				metap->level_heads[0]  = segment_root;
				metap->level_counts[0] = 1;
				metap->total_docs	   = total_docs;
				metap->total_len	   = total_len;

				MarkBufferDirty(metabuf);
				UnlockReleaseBuffer(metabuf);
			}

			/* Cleanup merge data */
			for (i = 0; i < num_merged_terms; i++)
			{
				if (merged_terms[i].term)
					pfree(merged_terms[i].term);
				if (merged_terms[i].segment_refs)
					pfree(merged_terms[i].segment_refs);
			}
			if (merged_terms)
				pfree(merged_terms);

			if (sink.writer.pages)
				pfree(sink.writer.pages);

			MemoryContextDelete(merge_ctx);
		}
		else
		{
			/* No segments at all — just update metapage stats */
			Buffer			metabuf;
			Page			metapage;
			TpIndexMetaPage metap;

			metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
			LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
			metapage = BufferGetPage(metabuf);
			metap	 = (TpIndexMetaPage)PageGetContents(metapage);

			metap->total_docs = total_docs;
			metap->total_len  = total_len;

			MarkBufferDirty(metabuf);
			UnlockReleaseBuffer(metabuf);
		}

		/*
		 * Close merge sources.  merge_source_close() frees term
		 * data and closes the underlying reader, so we only need
		 * to close readers that were not successfully wrapped in
		 * a source (e.g. empty segments that were skipped).
		 */
		for (i = 0; i < num_sources; i++)
			merge_source_close(&sources[i]);

		for (i = 0; i < total_segments; i++)
		{
			if (readers[i])
				tp_segment_close(readers[i]);
		}
		for (w = 0; w < launched; w++)
		{
			if (file_opened[w])
				BufFileClose(open_files[w]);
		}

		pfree(readers);
		pfree(sources);
		pfree(open_files);
		pfree(file_opened);
	}

	/* Wake workers so they can exit */
	pg_atomic_write_u32(&shared->phase2_ready, 1);
	ConditionVariableBroadcast(&shared->phase2_cv);

	/* Wait for all workers to complete */
	ConditionVariablePrepareToSleep(&shared->all_done_cv);
	while (pg_atomic_read_u32(&shared->workers_done) < (uint32)launched)
	{
		ConditionVariableTimedSleep(
				&shared->all_done_cv, 1000 /* ms */, PG_WAIT_EXTENSION);
	}
	ConditionVariableCancelSleep();
	WaitForParallelWorkersToFinish(pcxt);

	/* Build result */
	result				 = palloc0(sizeof(IndexBuildResult));
	result->heap_tuples	 = (double)total_docs;
	result->index_tuples = (double)total_docs;

	/* Cleanup */
	DestroyParallelContext(pcxt);
	ExitParallelMode();
#if PG_VERSION_NUM >= 180000
	UnregisterSnapshot(snapshot);
#endif

	return result;
}
