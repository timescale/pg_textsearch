/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build_parallel.c - Parallel index build implementation
 *
 * Architecture:
 * - Each worker scans a partition of the heap and builds a local
 *   TpBuildContext (arena + HTAB) — no shared memory allocations.
 * - When the worker's memory budget fills, it flushes a segment
 *   to a BufFile temp file via SharedFileSet.
 * - After all workers finish, the leader reads temp files and
 *   writes segments contiguously to index pages as L0 segments.
 * - No page pool, no compaction during build.
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <commands/progress.h>
#include <inttypes.h>
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
#include "segment/segment.h"
#include "state/metapage.h"

/* Defined in build.c, extracts terms from TSVector */
extern int tp_extract_terms_from_tsvector(
		TSVector tsvector,
		char  ***terms_out,
		int32  **frequencies_out,
		int		*term_count_out);

/* Chunk size for streaming temp segments to index pages */
#define TP_COPY_BUF_SIZE (64 * 1024)

/*
 * Estimate shared memory needed for parallel build
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

/*
 * Initialize shared state for parallel build
 */
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
	{
		results[i].segment_count  = 0;
		results[i].total_docs	  = 0;
		results[i].total_len	  = 0;
		results[i].tuples_scanned = 0;
	}
}

/*
 * Write a temp file segment to contiguous index pages.
 *
 * Reads data_size bytes from BufFile starting at base_offset,
 * writes them through a TpSegmentWriter to get proper 8KB pages
 * with page headers. Then writes the page index and updates the
 * segment header on the first page.
 *
 * Returns the header block number of the written segment.
 */
static BlockNumber
write_temp_segment_to_index(
		Relation index, BufFile *file, uint64 base_offset, uint64 data_size)
{
	TpSegmentWriter writer;
	BlockNumber		header_block;
	BlockNumber		page_index_root;
	char			buf[TP_COPY_BUF_SIZE];
	uint64			remaining;

	/* Initialize writer — extends relation via P_NEW */
	tp_segment_writer_init(&writer, index);
	header_block = writer.pages[0];

	/* Stream data from BufFile through writer */
	BufFileSeek(file, 0, (off_t)base_offset, SEEK_SET);
	remaining = data_size;
	while (remaining > 0)
	{
		uint32 chunk = (uint32)Min(remaining, TP_COPY_BUF_SIZE);

		BufFileReadExact(file, buf, chunk);
		tp_segment_writer_write(&writer, buf, chunk);
		remaining -= chunk;
	}

	/* Flush remaining buffered data */
	tp_segment_writer_flush(&writer);
	writer.buffer_pos = SizeOfPageHeaderData;

	/* Write page index */
	page_index_root =
			write_page_index(index, writer.pages, writer.pages_allocated);

	/*
	 * Update segment header on the first page.
	 * The logical offsets from the flat format are valid as-is because
	 * TpSegmentWriter.current_offset counts data bytes only (excludes
	 * page headers). We just need to set num_pages and page_index.
	 */
	{
		Buffer			 header_buf;
		Page			 header_page;
		TpSegmentHeader *hdr;

		header_buf = ReadBuffer(index, header_block);
		LockBuffer(header_buf, BUFFER_LOCK_EXCLUSIVE);
		header_page = BufferGetPage(header_buf);
		hdr			= (TpSegmentHeader *)PageGetContents(header_page);

		hdr->num_pages	= writer.pages_allocated;
		hdr->page_index = page_index_root;
		hdr->data_size	= writer.current_offset;

		MarkBufferDirty(header_buf);
		UnlockReleaseBuffer(header_buf);
	}

	tp_segment_writer_finish(&writer);

	/* Free writer pages array */
	if (writer.pages)
		pfree(writer.pages);

	return header_block;
}

/*
 * Worker entry point - called by parallel infrastructure
 *
 * Each worker:
 * 1. Opens heap and index relations
 * 2. Creates a local TpBuildContext with per-worker budget
 * 3. Scans its share of the heap
 * 4. Flushes segments to BufFile when budget fills
 * 5. Reports results via shared memory
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

	/* Attach to shared memory */
	shared = (TpParallelBuildShared *)
			shm_toc_lookup(toc, TP_PARALLEL_KEY_SHARED, false);

	worker_id = ParallelWorkerNumber;
	my_result = &TpParallelWorkerResults(shared)[worker_id];

	/* Open heap and index relations.
	 * Index uses AccessExclusiveLock matching btree nbtsort.c pattern:
	 * leader already holds this lock, workers inherit it via parallel
	 * context. This ensures proper smgr synchronization for concurrent
	 * relation extension.
	 */
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
	 * Per-worker memory budget: split maintenance_work_mem across workers.
	 * Minimum 64MB per worker to avoid excessive flushing.
	 */
	budget = (Size)maintenance_work_mem * 1024L / shared->nworkers;
	if (budget < 64L * 1024 * 1024)
		budget = 64L * 1024 * 1024;

	build_ctx = tp_build_context_create(budget);

	/*
	 * Per-document memory context for tokenization temporaries.
	 * Reset after each document to prevent unbounded growth.
	 */
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

		/* Reset per-doc context (frees tsvector, terms) */
		MemoryContextReset(build_tmpctx);

		/* Budget-based flush to BufFile */
		if (tp_build_context_should_flush(build_ctx))
		{
			my_result->total_docs += build_ctx->num_docs;
			my_result->total_len += build_ctx->total_len;

			tp_write_segment_to_buffile(build_ctx, buffile);
			my_result->segment_count++;
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
		my_result->total_docs += build_ctx->num_docs;
		my_result->total_len += build_ctx->total_len;

		tp_write_segment_to_buffile(build_ctx, buffile);
		my_result->segment_count++;
	}

	/* Export BufFile so leader can read it */
	BufFileExportFileSet(buffile);
	BufFileClose(buffile);

	/* Cleanup worker-local resources */
	tp_build_context_destroy(build_ctx);
	MemoryContextDelete(build_tmpctx);

	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	index_close(index, AccessExclusiveLock);
	table_close(heap, AccessShareLock);

	/* Signal completion */
	pg_atomic_fetch_add_u32(&shared->workers_done, 1);
	ConditionVariableSignal(&shared->all_done_cv);
}

/*
 * Main entry point for parallel index build
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
	int32				   total_segs = 0;

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
	 * Wait for workers, polling progress periodically.
	 * Workers atomically increment shared->tuples_done; we
	 * report it so pg_stat_progress_create_index stays live.
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

	/* Collect results from all workers */
	{
		TpParallelWorkerResult *results;
		int						i;

		results = TpParallelWorkerResults(shared);
		for (i = 0; i < launched; i++)
		{
			total_docs += results[i].total_docs;
			total_len += results[i].total_len;
			total_segs += results[i].segment_count;
		}
	}

	/* Report final tuple count */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_TUPLES_DONE, (int64)total_docs);

	/* Report writing phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_COMPACTING);

	/*
	 * Read each worker's temp segments and write them contiguously
	 * to index pages. Since the index only has block 0 (metapage)
	 * at this point, P_NEW gives sequential blocks 1, 2, 3, ...
	 * All segments end up perfectly contiguous.
	 */
	{
		Buffer			metabuf;
		Page			metapage;
		TpIndexMetaPage metap;
		BlockNumber		l0_head = InvalidBlockNumber;
		BlockNumber		l0_tail = InvalidBlockNumber;
		int				i;

		TpParallelWorkerResult *results = TpParallelWorkerResults(shared);

		for (i = 0; i < launched; i++)
		{
			char	 file_name[64];
			BufFile *file;
			uint64	 offset;
			uint32	 s;

			if (results[i].segment_count == 0)
				continue;

			snprintf(file_name, sizeof(file_name), "tp_worker_%d", i);
			file = BufFileOpenFileSet(
					&shared->fileset.fs, file_name, O_RDONLY, false);

			offset = 0;
			for (s = 0; s < results[i].segment_count; s++)
			{
				TpSegmentHeader hdr;
				BlockNumber		seg_root;

				/* Read header to get data_size */
				BufFileSeek(file, 0, (off_t)offset, SEEK_SET);
				BufFileReadExact(file, &hdr, sizeof(TpSegmentHeader));

				/* Write temp segment to index pages */
				seg_root = write_temp_segment_to_index(
						index, file, offset, hdr.data_size);

				/* Chain into L0 linked list */
				if (l0_tail != InvalidBlockNumber)
				{
					/* Link previous tail to this segment */
					Buffer			 tail_buf;
					Page			 tail_page;
					TpSegmentHeader *tail_hdr;

					tail_buf = ReadBuffer(index, l0_tail);
					LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
					tail_page = BufferGetPage(tail_buf);
					tail_hdr  = (TpSegmentHeader *)PageGetContents(tail_page);
					tail_hdr->next_segment = seg_root;
					MarkBufferDirty(tail_buf);
					UnlockReleaseBuffer(tail_buf);
				}
				else
				{
					l0_head = seg_root;
				}
				l0_tail = seg_root;

				offset += hdr.data_size;
			}

			BufFileClose(file);
		}

		/* Update metapage with L0 segments */
		metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
		LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
		metapage = BufferGetPage(metabuf);
		metap	 = (TpIndexMetaPage)PageGetContents(metapage);

		metap->level_heads[0]  = l0_head;
		metap->level_counts[0] = total_segs;
		metap->total_docs	   = total_docs;
		metap->total_len	   = total_len;

		MarkBufferDirty(metabuf);
		UnlockReleaseBuffer(metabuf);
	}

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
