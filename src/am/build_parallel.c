/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.c - Parallel index build
 *
 * Architecture:
 * - Workers scan heap partitions in parallel, build local memtables,
 *   flush segments to BufFile, and compact within BufFile.
 *   Workers report segment offsets/sizes/levels and signal done.
 * - Leader reopens each worker's BufFile, creates merge sources for
 *   every segment, performs a single N-way merge into index pages
 *   via TpMergeSink. This produces one contiguous segment with zero
 *   fragmentation.
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <catalog/storage.h>
#include <commands/progress.h>
#include <miscadmin.h>
#include <storage/buffile.h>
#include <storage/bufmgr.h>
#include <storage/condition_variable.h>
#include <storage/smgr.h>
#include <tsearch/ts_type.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>
#include <utils/wait_event.h>

#include "am.h"
#include "build_context.h"
#include "build_parallel.h"
#include "constants.h"
#include "segment/compression.h"
#include "segment/docmap.h"
#include "segment/fieldnorm.h"
#include "segment/merge.h"
#include "segment/merge_internal.h"
#include "segment/pagemapper.h"
#include "segment/segment.h"
#include "state/metapage.h"

/* Defined in build.c, extracts terms from TSVector */
extern int tp_extract_terms_from_tsvector(
		TSVector tsvector,
		char  ***terms_out,
		int32  **frequencies_out,
		int		*term_count_out);

/* GUC variable for segment compression */
extern bool tp_compress_segments;

/* ----------------------------------------------------------------
 * Worker-local segment tracking
 * ----------------------------------------------------------------
 */

typedef struct WorkerSegmentEntry
{
	uint64 offset;	  /* BufFile offset */
	uint64 data_size; /* Segment bytes */
	uint32 level;	  /* 0=L0, 1=L1, etc. */
	bool   consumed;  /* merged into higher level */
} WorkerSegmentEntry;

typedef struct WorkerSegmentTracker
{
	WorkerSegmentEntry *entries;
	uint32				count;
	uint32				capacity;
	uint32				level_counts[TP_MAX_LEVELS];
	uint64				buffile_end; /* logical end of BufFile */
} WorkerSegmentTracker;

static void
tracker_init(WorkerSegmentTracker *tracker)
{
	tracker->capacity	 = 32;
	tracker->count		 = 0;
	tracker->buffile_end = 0;
	tracker->entries = palloc(tracker->capacity * sizeof(WorkerSegmentEntry));
	memset(tracker->level_counts, 0, sizeof(tracker->level_counts));
}

static void
tracker_add_segment(
		WorkerSegmentTracker *tracker,
		uint64				  offset,
		uint64				  data_size,
		uint32				  level)
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
	tracker->entries[tracker->count].level	   = level;
	tracker->entries[tracker->count].consumed  = false;
	tracker->count++;
	tracker->level_counts[level]++;
}

static void
tracker_destroy(WorkerSegmentTracker *tracker)
{
	if (tracker->entries)
		pfree(tracker->entries);
}

/* ----------------------------------------------------------------
 * Worker-side BufFile compaction
 *
 * Mirrors tp_maybe_compact_level() but operates entirely within
 * BufFile. Opens BufFile readers for source segments, performs
 * N-way merge, writes result back to BufFile using TpMergeSink.
 * ----------------------------------------------------------------
 */
static void
worker_maybe_compact_level(
		WorkerSegmentTracker *tracker, BufFile *file, uint32 level)
{
	uint32 spl = (uint32)tp_segments_per_level;

	if (level >= TP_MAX_LEVELS - 1)
		return;

	while (tracker->level_counts[level] >= spl)
	{
		TpMergeSource	 *sources;
		TpSegmentReader **readers;
		int				  num_sources;
		TpMergedTerm	 *merged_terms	   = NULL;
		uint32			  num_merged_terms = 0;
		uint32			  merged_capacity  = 0;
		uint64			  total_tokens	   = 0;
		uint32			  collected		   = 0;
		uint32			  i;
		MemoryContext	  merge_ctx;
		MemoryContext	  old_ctx;

		/* Collect spl non-consumed entries at this level */
		sources		= palloc0(sizeof(TpMergeSource) * spl);
		readers		= palloc0(sizeof(TpSegmentReader *) * spl);
		num_sources = 0;

		for (i = 0; i < tracker->count && collected < spl; i++)
		{
			WorkerSegmentEntry *e = &tracker->entries[i];

			if (e->consumed || e->level != level)
				continue;

			readers[collected] = tp_segment_open_from_buffile(file, e->offset);
			if (!readers[collected])
				continue;

			if (merge_source_init_from_reader(
						&sources[num_sources], readers[collected]))
			{
				total_tokens += readers[collected]->header->total_tokens;
				num_sources++;
			}
			collected++;
		}

		if (num_sources < 2)
		{
			/* Not enough valid sources to merge */
			for (i = 0; i < (uint32)num_sources; i++)
				merge_source_close(&sources[i]);
			for (i = 0; i < collected; i++)
			{
				if (readers[i])
					tp_segment_close(readers[i]);
			}
			pfree(sources);
			pfree(readers);
			break;
		}

		merge_ctx = AllocSetContextCreate(
				CurrentMemoryContext,
				"Worker BufFile Merge",
				ALLOCSET_DEFAULT_SIZES);
		old_ctx = MemoryContextSwitchTo(merge_ctx);

		/* N-way term merge */
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
				merged_capacity = merged_capacity == 0 ? 1024
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

			current_merged					 = &merged_terms[num_merged_terms];
			current_merged->term_len		 = strlen(min_term);
			current_merged->term			 = pstrdup(min_term);
			current_merged->segment_refs	 = NULL;
			current_merged->num_segment_refs = 0;
			current_merged->segment_refs_capacity = 0;
			current_merged->posting_offset		  = 0;
			current_merged->posting_count		  = 0;
			num_merged_terms++;

			for (i = 0; i < (uint32)num_sources; i++)
			{
				if (sources[i].exhausted)
					continue;

				if (strcmp(sources[i].current_term, current_merged->term) == 0)
				{
					merged_term_add_segment_ref(
							current_merged, i, &sources[i].current_entry);
					merge_source_advance(&sources[i]);
				}
			}

			CHECK_FOR_INTERRUPTS();
		}

		MemoryContextSwitchTo(old_ctx);

		/* Write merged segment to BufFile via sink */
		if (num_merged_terms > 0)
		{
			TpMergeSink sink;
			int			end_fileno;
			off_t		end_offset;
			uint64		new_offset;
			uint64		new_data_size;

			/*
			 * Position at logical end of BufFile for appending.
			 * Do NOT use SEEK_END: BufFileSeek(SEEK_END) calls
			 * FileSize() before flushing the dirty buffer, so it
			 * returns the on-disk size which may be less than the
			 * logical size. Use our tracked buffile_end instead.
			 */
			new_offset = tracker->buffile_end;
			tp_buffile_decompose_offset(new_offset, &end_fileno, &end_offset);
			BufFileSeek(file, end_fileno, end_offset, SEEK_SET);

			merge_sink_init_buffile(&sink, file);
			write_merged_segment_to_sink(
					&sink,
					merged_terms,
					num_merged_terms,
					sources,
					num_sources,
					level + 1,
					total_tokens);
			new_data_size = sink.current_offset;

			/* Update logical end of BufFile */
			tracker->buffile_end = new_offset + new_data_size;

			/* Mark source segments consumed */
			collected = 0;
			for (i = 0; i < tracker->count && collected < spl; i++)
			{
				WorkerSegmentEntry *e = &tracker->entries[i];

				if (e->consumed || e->level != level)
					continue;

				e->consumed = true;
				collected++;
			}
			tracker->level_counts[level] -= spl;

			/* Add merged segment */
			tracker_add_segment(tracker, new_offset, new_data_size, level + 1);
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

		/* Close sources (don't close readers - merge_source_close
		 * doesn't own them for init_from_reader) */
		for (i = 0; i < (uint32)num_sources; i++)
		{
			if (sources[i].current_term)
				pfree(sources[i].current_term);
			if (sources[i].string_offsets)
				pfree(sources[i].string_offsets);
			sources[i].reader = NULL;
		}
		for (i = 0; i < collected; i++)
		{
			if (readers[i])
				tp_segment_close(readers[i]);
		}

		pfree(sources);
		pfree(readers);
		MemoryContextDelete(merge_ctx);
	}

	/* Recurse to check next level */
	worker_maybe_compact_level(tracker, file, level + 1);
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

	/* Base shared structure */
	size = MAXALIGN(sizeof(TpParallelBuildShared));

	/* Per-worker result array */
	size = add_size(size, MAXALIGN(sizeof(TpParallelWorkerResult) * nworkers));

	/* Parallel table scan descriptor */
	size = add_size(size, table_parallelscan_estimate(heap, snapshot));

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

	/* Join parallel table scan */
	scan = table_beginscan_parallel(heap, TpParallelTableScan(shared));
	slot = table_slot_create(heap, NULL);

	/*
	 * Per-worker memory budget: split maintenance_work_mem across
	 * workers. Minimum 64MB per worker to avoid excessive flushing.
	 */
	budget = (Size)maintenance_work_mem * 1024L / shared->nworkers;
	if (budget < 64L * 1024 * 1024)
		budget = 64L * 1024 * 1024;

	build_ctx = tp_build_context_create(budget);
	tracker_init(&tracker);

	build_tmpctx = AllocSetContextCreate(
			CurrentMemoryContext,
			"parallel build per-doc temp",
			ALLOCSET_DEFAULT_SIZES);

	/* Process tuples */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
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
			my_result->segment_count++;

			/* Track this L0 segment */
			tracker_add_segment(&tracker, seg_offset, data_size, 0);
			tracker.buffile_end = seg_offset + data_size;

			tp_build_context_reset(build_ctx);

			/* Run compaction if L0 is full */
			worker_maybe_compact_level(&tracker, buffile, 0);
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
		my_result->segment_count++;

		tracker_add_segment(&tracker, seg_offset, data_size, 0);
		tracker.buffile_end = seg_offset + data_size;

		/* Final compaction pass */
		worker_maybe_compact_level(&tracker, buffile, 0);
	}

	/*
	 * Report non-consumed segments to leader.
	 * These are the final segments after worker-side compaction.
	 */
	{
		uint32 seg_idx = 0;
		uint32 i;

		for (i = 0; i < tracker.count; i++)
		{
			WorkerSegmentEntry *e = &tracker.entries[i];

			if (e->consumed)
				continue;

			if (seg_idx >= TP_MAX_WORKER_SEGMENTS)
			{
				elog(WARNING,
					 "worker %d has too many segments (%u), "
					 "truncating",
					 worker_id,
					 tracker.count);
				break;
			}

			my_result->seg_offsets[seg_idx] = e->offset;
			my_result->seg_sizes[seg_idx]	= e->data_size;
			my_result->seg_levels[seg_idx]	= e->level;
			seg_idx++;
		}
		my_result->final_segment_count = seg_idx;
	}

	/* Export BufFile so leader can reopen */
	BufFileExportFileSet(buffile);
	BufFileClose(buffile);

	/* Cleanup */
	tp_build_context_destroy(build_ctx);
	tracker_destroy(&tracker);
	MemoryContextDelete(build_tmpctx);

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);

	index_close(index, AccessExclusiveLock);
	table_close(heap, AccessShareLock);

	/* Signal done */
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

	/* Initialize parallel table scan */
	table_parallelscan_initialize(heap, TpParallelTableScan(shared), snapshot);

	/* Insert shared state into TOC */
	shm_toc_insert(pcxt->toc, TP_PARALLEL_KEY_SHARED, shared);

	/* Launch workers */
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;

	ereport(NOTICE,
			(errmsg("parallel index build: launched %d of %d "
					"requested workers",
					launched,
					nworkers)));

	/*
	 * Wait for all workers to finish BufFile phase and exit.
	 */
	ConditionVariablePrepareToSleep(&shared->all_done_cv);
	while (pg_atomic_read_u32(&shared->workers_done) < (uint32)launched)
	{
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_DONE,
				(int64)pg_atomic_read_u64(&shared->tuples_done));

		ConditionVariableTimedSleep(
				&shared->all_done_cv, 1000 /* ms */, PG_WAIT_EXTENSION);
	}
	ConditionVariableCancelSleep();
	WaitForParallelWorkersToFinish(pcxt);

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

	/* Report compaction phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_COMPACTING);

	/*
	 * Leader merge: open all worker BufFiles, create merge sources
	 * for every segment, N-way merge into a single pages-backed
	 * segment via TpMergeSink.
	 */
	{
		TpParallelWorkerResult *results = TpParallelWorkerResults(shared);
		int						total_segments = 0;
		int						i;
		TpSegmentReader		  **all_readers;
		TpMergeSource		   *sources;
		int						num_sources;
		BufFile				  **worker_files;
		TpMergedTerm		   *merged_terms	   = NULL;
		uint32					num_merged_terms   = 0;
		uint32					merged_capacity	   = 0;
		uint64					merge_total_tokens = 0;
		MemoryContext			merge_ctx;
		MemoryContext			old_ctx;

		/* Count total segments across all workers */
		for (i = 0; i < launched; i++)
			total_segments += results[i].final_segment_count;

		if (total_segments == 0)
			goto done_merge;

		/* Open all worker BufFiles */
		worker_files = palloc(launched * sizeof(BufFile *));
		for (i = 0; i < launched; i++)
		{
			char name[64];

			snprintf(name, sizeof(name), "tp_worker_%d", i);
			worker_files[i] = BufFileOpenFileSet(
					&shared->fileset.fs, name, O_RDONLY, false);
		}

		/* Create readers and merge sources for all segments */
		all_readers = palloc0(total_segments * sizeof(TpSegmentReader *));
		sources		= palloc0(total_segments * sizeof(TpMergeSource));
		num_sources = 0;

		for (i = 0; i < launched; i++)
		{
			uint32 s;

			for (s = 0; s < results[i].final_segment_count; s++)
			{
				TpSegmentReader *reader;

				reader = tp_segment_open_from_buffile(
						worker_files[i], results[i].seg_offsets[s]);
				if (!reader)
					continue;

				all_readers[num_sources] = reader;

				if (merge_source_init_from_reader(
							&sources[num_sources], reader))
				{
					merge_total_tokens += reader->header->total_tokens;
					num_sources++;
				}
				else
				{
					tp_segment_close(reader);
					all_readers[num_sources] = NULL;
				}
			}
		}

		if (num_sources == 0)
		{
			/* Cleanup and skip merge */
			pfree(sources);
			pfree(all_readers);
			for (i = 0; i < launched; i++)
				BufFileClose(worker_files[i]);
			pfree(worker_files);
			goto done_merge;
		}

		/* N-way term merge */
		merge_ctx = AllocSetContextCreate(
				CurrentMemoryContext, "Leader Merge", ALLOCSET_DEFAULT_SIZES);
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
				merged_capacity = merged_capacity == 0 ? 1024
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

			current_merged					 = &merged_terms[num_merged_terms];
			current_merged->term_len		 = strlen(min_term);
			current_merged->term			 = pstrdup(min_term);
			current_merged->segment_refs	 = NULL;
			current_merged->num_segment_refs = 0;
			current_merged->segment_refs_capacity = 0;
			current_merged->posting_offset		  = 0;
			current_merged->posting_count		  = 0;
			num_merged_terms++;

			for (i = 0; i < num_sources; i++)
			{
				if (sources[i].exhausted)
					continue;

				if (strcmp(sources[i].current_term, current_merged->term) == 0)
				{
					merged_term_add_segment_ref(
							current_merged, i, &sources[i].current_entry);
					merge_source_advance(&sources[i]);
				}
			}

			CHECK_FOR_INTERRUPTS();
		}

		/* Write merged segment to index pages via sink */
		if (num_merged_terms > 0)
		{
			TpMergeSink		sink;
			BlockNumber		new_segment;
			Buffer			metabuf;
			Page			metapage;
			TpIndexMetaPage metap;

			/* Report writing phase */
			pgstat_progress_update_param(
					PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_WRITING);

			merge_sink_init_pages(&sink, index);
			new_segment = sink.writer.pages[0];

			write_merged_segment_to_sink(
					&sink,
					merged_terms,
					num_merged_terms,
					sources,
					num_sources,
					1, /* target level */
					merge_total_tokens);

			if (sink.writer.pages)
				pfree(sink.writer.pages);

			/* Update metapage: one L1 segment */
			metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
			LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
			metapage = BufferGetPage(metabuf);
			metap	 = (TpIndexMetaPage)PageGetContents(metapage);

			metap->level_heads[1]  = new_segment;
			metap->level_counts[1] = 1;
			metap->total_docs	   = total_docs;
			metap->total_len	   = total_len;

			MarkBufferDirty(metabuf);
			UnlockReleaseBuffer(metabuf);
		}

		/* Cleanup merge data */
		for (i = 0; i < (int)num_merged_terms; i++)
		{
			if (merged_terms[i].term)
				pfree(merged_terms[i].term);
			if (merged_terms[i].segment_refs)
				pfree(merged_terms[i].segment_refs);
		}
		if (merged_terms)
			pfree(merged_terms);

		/* Close merge sources and readers */
		for (i = 0; i < num_sources; i++)
		{
			if (sources[i].current_term)
				pfree(sources[i].current_term);
			if (sources[i].string_offsets)
				pfree(sources[i].string_offsets);
			sources[i].reader = NULL;
		}
		for (i = 0; i < total_segments; i++)
		{
			if (all_readers[i])
				tp_segment_close(all_readers[i]);
		}
		pfree(sources);
		pfree(all_readers);

		/* Close worker BufFiles */
		for (i = 0; i < launched; i++)
			BufFileClose(worker_files[i]);
		pfree(worker_files);

		MemoryContextSwitchTo(old_ctx);
		MemoryContextDelete(merge_ctx);
	}

done_merge:
	/* Flush all relation buffers to disk */
	FlushRelationBuffers(index);

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
