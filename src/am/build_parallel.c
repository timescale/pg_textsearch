/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am/build_parallel.c - Parallel index build implementation
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/table.h>
#include <access/tableam.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <commands/progress.h>
#include <miscadmin.h>
#include <storage/bufmgr.h>
#include <storage/condition_variable.h>
#include <storage/smgr.h>
#include <tsearch/ts_type.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "am.h"
#include "build_parallel.h"
#include "constants.h"
#include "memtable/local_memtable.h"
#include "segment/docmap.h"
#include "segment/segment.h"
#include "state/metapage.h"

/*
 * Callback wrapper for building docmap from local memtable
 */
static void
docmap_add_callback(ItemPointer ctid, int32 doc_length, void *arg)
{
	TpDocMapBuilder *docmap = (TpDocMapBuilder *)arg;
	tp_docmap_add(docmap, ctid, (uint32)doc_length);
}

/* Minimum pages to pre-allocate per worker */
#define TP_MIN_PAGES_PER_WORKER 64

/* Default expansion factor for estimating index pages from heap */
#define TP_INDEX_EXPANSION_FACTOR 2.0

/*
 * Forward declarations for local functions
 */
static void tp_init_parallel_shared(
		TpParallelBuildShared *shared,
		Relation			   heap,
		Relation			   index,
		Oid					   text_config_oid,
		double				   k1,
		double				   b,
		int					   nworkers);

static void tp_preallocate_page_pools(
		Relation index, TpParallelBuildShared *shared, int pages_per_worker);

static void tp_leader_participate(
		TpParallelBuildShared *shared,
		Relation			   heap,
		Relation			   index,
		Snapshot			   snapshot);

static void
tp_link_all_worker_segments(TpParallelBuildShared *shared, Relation index);

static void
tp_finalize_parallel_stats(TpParallelBuildShared *shared, Relation index);

static BlockNumber tp_write_segment_from_local_memtable(
		TpLocalMemtable		  *memtable,
		Relation			   index,
		TpParallelBuildShared *shared,
		int					   worker_id);

static void tp_worker_process_document(
		TpLocalMemtable *memtable,
		TupleTableSlot	*slot,
		int				 attnum,
		Oid				 text_config_oid);

/*
 * Estimate shared memory needed for parallel build
 */
Size
tp_parallel_build_estimate_shmem(
		Relation heap, Snapshot snapshot, int nworkers, int pages_per_worker)
{
	Size size;

	/* Base shared structure */
	size = MAXALIGN(sizeof(TpParallelBuildShared));

	/* Per-worker segment info array */
	size = add_size(size, MAXALIGN(sizeof(TpWorkerSegmentInfo) * nworkers));

	/* Page pools for all workers */
	size = add_size(
			size,
			MAXALIGN((Size)nworkers * pages_per_worker * sizeof(BlockNumber)));

	/* Parallel table scan descriptor */
	size = add_size(size, table_parallelscan_estimate(heap, snapshot));

	return size;
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
	int					   pages_per_worker;
	BlockNumber			   heap_pages;
	int					   launched;

	/* Ensure reasonable number of workers */
	if (nworkers > TP_MAX_PARALLEL_WORKERS)
		nworkers = TP_MAX_PARALLEL_WORKERS;

	/* Estimate pages needed per worker */
	heap_pages		 = RelationGetNumberOfBlocks(heap);
	pages_per_worker = (int)((heap_pages * TP_INDEX_EXPANSION_FACTOR) /
							 nworkers) +
					   TP_MIN_PAGES_PER_WORKER;

	/* Get snapshot for parallel scan */
	snapshot = GetTransactionSnapshot();
	snapshot = RegisterSnapshot(snapshot);

	/* Calculate shared memory size */
	shmem_size = tp_parallel_build_estimate_shmem(
			heap, snapshot, nworkers, pages_per_worker);

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
			shared, heap, index, text_config_oid, k1, b, nworkers);
	shared->pages_per_worker = pages_per_worker;

	/* Pre-allocate page pools */
	tp_preallocate_page_pools(index, shared, pages_per_worker);

	/* Initialize parallel table scan */
	table_parallelscan_initialize(heap, TpParallelTableScan(shared), snapshot);

	/* Insert shared state into TOC */
	shm_toc_insert(pcxt->toc, TP_PARALLEL_KEY_SHARED, shared);

	/* Launch workers */
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;

	elog(DEBUG1,
		 "Parallel index build: requested %d workers, launched %d",
		 nworkers,
		 launched);

	/* Leader participates as a worker too */
	if (launched > 0)
	{
		shared->leader_working = true;
		tp_leader_participate(shared, heap, index, snapshot);
	}
	else
	{
		/* No workers launched - fall back would be handled by caller */
		elog(WARNING, "No parallel workers launched for index build");
	}

	/* Wait for all workers to finish */
	WaitForParallelWorkersToFinish(pcxt);

	/* Link all worker segments into L0 chain */
	tp_link_all_worker_segments(shared, index);

	/* Finalize statistics in metapage */
	tp_finalize_parallel_stats(shared, index);

	/* Build result */
	result				 = palloc0(sizeof(IndexBuildResult));
	result->heap_tuples	 = (double)pg_atomic_read_u64(&shared->tuples_scanned);
	result->index_tuples = (double)pg_atomic_read_u64(&shared->total_docs);

	/* Cleanup */
	DestroyParallelContext(pcxt);
	ExitParallelMode();
	UnregisterSnapshot(snapshot);

	return result;
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
		double				   k1,
		double				   b,
		int					   nworkers)
{
	TpWorkerSegmentInfo *worker_info;
	int					 i;

	memset(shared, 0, sizeof(TpParallelBuildShared));

	/* Immutable configuration */
	shared->heaprelid		= RelationGetRelid(heap);
	shared->indexrelid		= RelationGetRelid(index);
	shared->text_config_oid = text_config_oid;
	shared->k1				= k1;
	shared->b				= b;
	shared->worker_count	= nworkers;

	/* Per-worker spill threshold */
	shared->spill_threshold_per_worker = tp_memtable_spill_threshold /
										 nworkers;
	if (shared->spill_threshold_per_worker < 100000)
		shared->spill_threshold_per_worker = 100000; /* minimum */

	/* Initialize coordination primitives */
	SpinLockInit(&shared->mutex);
	ConditionVariableInit(&shared->workersdonecv);
	shared->workers_done   = 0;
	shared->leader_working = false;

	/* Initialize atomic counters */
	pg_atomic_init_u64(&shared->tuples_scanned, 0);
	pg_atomic_init_u64(&shared->total_docs, 0);
	pg_atomic_init_u64(&shared->total_len, 0);
	pg_atomic_init_u32(&shared->pool_exhausted, 0);

	/* Initialize per-worker pool indices */
	for (i = 0; i < nworkers && i < TP_MAX_PARALLEL_WORKERS; i++)
		pg_atomic_init_u32(&shared->pool_next[i], 0);

	/* Initialize worker segment info */
	worker_info = TpParallelWorkerInfo(shared);
	for (i = 0; i < nworkers; i++)
	{
		worker_info[i].segment_head	 = InvalidBlockNumber;
		worker_info[i].segment_tail	 = InvalidBlockNumber;
		worker_info[i].segment_count = 0;
		worker_info[i].docs_indexed	 = 0;
		worker_info[i].total_len	 = 0;
	}
}

/*
 * Pre-allocate pages for each worker's pool
 */
static void
tp_preallocate_page_pools(
		Relation index, TpParallelBuildShared *shared, int pages_per_worker)
{
	int			 total_pages;
	int			 i;
	Buffer		 buf;
	BlockNumber *all_pages;

	total_pages = shared->worker_count * pages_per_worker;
	all_pages	= (BlockNumber *)TpParallelPagePool(shared, 0);

	elog(DEBUG1,
		 "Pre-allocating %d pages (%d per worker) for parallel build",
		 total_pages,
		 pages_per_worker);

	/* Extend relation and collect block numbers */
	for (i = 0; i < total_pages; i++)
	{
		buf = ReadBufferExtended(
				index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
		all_pages[i] = BufferGetBlockNumber(buf);

		/* Initialize page */
		PageInit(BufferGetPage(buf), BLCKSZ, 0);
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
	}

	/* Flush to ensure durability */
	smgrimmedsync(RelationGetSmgr(index), MAIN_FORKNUM);
}

/*
 * Worker entry point - called by parallel infrastructure
 */
void
tp_parallel_build_worker_main(dsm_segment *seg, shm_toc *toc)
{
	TpParallelBuildShared *shared;
	TpWorkerSegmentInfo	  *my_info;
	Relation			   heap;
	Relation			   index;
	TableScanDesc		   scan;
	TupleTableSlot		  *slot;
	TpLocalMemtable		  *memtable;
	int					   worker_id;
	int64				   tuples_processed = 0;

	/* Attach to shared memory */
	shared = (TpParallelBuildShared *)
			shm_toc_lookup(toc, TP_PARALLEL_KEY_SHARED, false);

	worker_id = ParallelWorkerNumber;
	my_info	  = &TpParallelWorkerInfo(shared)[worker_id];

	elog(DEBUG1, "Parallel build worker %d starting", worker_id);

	/* Open relations */
	heap  = table_open(shared->heaprelid, AccessShareLock);
	index = index_open(shared->indexrelid, RowExclusiveLock);

	/* Create local memtable */
	memtable = tp_local_memtable_create();

	/* Join parallel table scan */
	scan = table_beginscan_parallel(heap, TpParallelTableScan(shared));
	slot = table_slot_create(heap, NULL);

	/* Process tuples */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		tp_worker_process_document(
				memtable,
				slot,
				1, /* first column */
				shared->text_config_oid);

		tuples_processed++;
		pg_atomic_fetch_add_u64(&shared->tuples_scanned, 1);

		/* Check spill threshold */
		if (memtable->total_postings >= shared->spill_threshold_per_worker)
		{
			BlockNumber seg_block;

			seg_block = tp_write_segment_from_local_memtable(
					memtable, index, shared, worker_id);

			if (seg_block != InvalidBlockNumber)
			{
				/* Chain segment into worker's list */
				if (my_info->segment_head == InvalidBlockNumber)
				{
					my_info->segment_head = seg_block;
					my_info->segment_tail = seg_block;
				}
				else
				{
					/* Link previous tail to new segment */
					Buffer			 tail_buf;
					Page			 tail_page;
					TpSegmentHeader *tail_header;

					tail_buf = ReadBuffer(index, my_info->segment_tail);
					LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
					tail_page	= BufferGetPage(tail_buf);
					tail_header = (TpSegmentHeader *)((char *)tail_page +
													  SizeOfPageHeaderData);
					tail_header->next_segment = seg_block;
					MarkBufferDirty(tail_buf);
					UnlockReleaseBuffer(tail_buf);

					my_info->segment_tail = seg_block;
				}
				my_info->segment_count++;

				/* Aggregate stats before clearing */
				my_info->docs_indexed += memtable->num_docs;
				my_info->total_len += memtable->total_len;
			}

			tp_local_memtable_clear(memtable);
		}

		CHECK_FOR_INTERRUPTS();
	}

	/* Final spill of remaining data */
	if (memtable->num_docs > 0)
	{
		BlockNumber seg_block;

		seg_block = tp_write_segment_from_local_memtable(
				memtable, index, shared, worker_id);

		if (seg_block != InvalidBlockNumber)
		{
			if (my_info->segment_head == InvalidBlockNumber)
			{
				my_info->segment_head = seg_block;
				my_info->segment_tail = seg_block;
			}
			else
			{
				Buffer			 tail_buf;
				Page			 tail_page;
				TpSegmentHeader *tail_header;

				tail_buf = ReadBuffer(index, my_info->segment_tail);
				LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
				tail_page	= BufferGetPage(tail_buf);
				tail_header = (TpSegmentHeader *)((char *)tail_page +
												  SizeOfPageHeaderData);
				tail_header->next_segment = seg_block;
				MarkBufferDirty(tail_buf);
				UnlockReleaseBuffer(tail_buf);

				my_info->segment_tail = seg_block;
			}
			my_info->segment_count++;
		}

		my_info->docs_indexed += memtable->num_docs;
		my_info->total_len += memtable->total_len;
	}

	/* Update global stats */
	pg_atomic_fetch_add_u64(&shared->total_docs, my_info->docs_indexed);
	pg_atomic_fetch_add_u64(&shared->total_len, my_info->total_len);

	/* Signal completion */
	SpinLockAcquire(&shared->mutex);
	shared->workers_done++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->workersdonecv);

	elog(DEBUG1,
		 "Worker %d done: %ld tuples, %d segments, %ld docs",
		 worker_id,
		 (long)tuples_processed,
		 my_info->segment_count,
		 (long)my_info->docs_indexed);

	/* Cleanup */
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	tp_local_memtable_destroy(memtable);
	index_close(index, RowExclusiveLock);
	table_close(heap, AccessShareLock);
}

/*
 * Leader participates as worker 0
 */
static void
tp_leader_participate(
		TpParallelBuildShared *shared,
		Relation			   heap,
		Relation			   index,
		Snapshot			   snapshot)
{
	TpWorkerSegmentInfo *my_info;
	TableScanDesc		 scan;
	TupleTableSlot		*slot;
	TpLocalMemtable		*memtable;
	int64				 tuples_processed = 0;
	int					 worker_id		  = 0; /* leader is worker 0 */

	my_info = &TpParallelWorkerInfo(shared)[worker_id];

	elog(DEBUG1, "Leader participating as worker %d", worker_id);

	/* Create local memtable */
	memtable = tp_local_memtable_create();

	/* Join parallel table scan */
	scan = table_beginscan_parallel(heap, TpParallelTableScan(shared));
	slot = table_slot_create(heap, NULL);

	/* Process tuples */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		tp_worker_process_document(memtable, slot, 1, shared->text_config_oid);

		tuples_processed++;
		pg_atomic_fetch_add_u64(&shared->tuples_scanned, 1);

		/* Check spill threshold */
		if (memtable->total_postings >= shared->spill_threshold_per_worker)
		{
			BlockNumber seg_block;

			seg_block = tp_write_segment_from_local_memtable(
					memtable, index, shared, worker_id);

			if (seg_block != InvalidBlockNumber)
			{
				if (my_info->segment_head == InvalidBlockNumber)
				{
					my_info->segment_head = seg_block;
					my_info->segment_tail = seg_block;
				}
				else
				{
					Buffer			 tail_buf;
					Page			 tail_page;
					TpSegmentHeader *tail_header;

					tail_buf = ReadBuffer(index, my_info->segment_tail);
					LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
					tail_page	= BufferGetPage(tail_buf);
					tail_header = (TpSegmentHeader *)((char *)tail_page +
													  SizeOfPageHeaderData);
					tail_header->next_segment = seg_block;
					MarkBufferDirty(tail_buf);
					UnlockReleaseBuffer(tail_buf);

					my_info->segment_tail = seg_block;
				}
				my_info->segment_count++;

				my_info->docs_indexed += memtable->num_docs;
				my_info->total_len += memtable->total_len;
			}

			tp_local_memtable_clear(memtable);
		}

		CHECK_FOR_INTERRUPTS();
	}

	/* Final spill */
	if (memtable->num_docs > 0)
	{
		BlockNumber seg_block;

		seg_block = tp_write_segment_from_local_memtable(
				memtable, index, shared, worker_id);

		if (seg_block != InvalidBlockNumber)
		{
			if (my_info->segment_head == InvalidBlockNumber)
			{
				my_info->segment_head = seg_block;
				my_info->segment_tail = seg_block;
			}
			else
			{
				Buffer			 tail_buf;
				Page			 tail_page;
				TpSegmentHeader *tail_header;

				tail_buf = ReadBuffer(index, my_info->segment_tail);
				LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
				tail_page	= BufferGetPage(tail_buf);
				tail_header = (TpSegmentHeader *)((char *)tail_page +
												  SizeOfPageHeaderData);
				tail_header->next_segment = seg_block;
				MarkBufferDirty(tail_buf);
				UnlockReleaseBuffer(tail_buf);

				my_info->segment_tail = seg_block;
			}
			my_info->segment_count++;
		}

		my_info->docs_indexed += memtable->num_docs;
		my_info->total_len += memtable->total_len;
	}

	/* Update global stats */
	pg_atomic_fetch_add_u64(&shared->total_docs, my_info->docs_indexed);
	pg_atomic_fetch_add_u64(&shared->total_len, my_info->total_len);

	/* Signal completion */
	SpinLockAcquire(&shared->mutex);
	shared->workers_done++;
	SpinLockRelease(&shared->mutex);
	ConditionVariableSignal(&shared->workersdonecv);

	elog(DEBUG1,
		 "Leader done: %ld tuples, %d segments, %ld docs",
		 (long)tuples_processed,
		 my_info->segment_count,
		 (long)my_info->docs_indexed);

	/* Cleanup */
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	tp_local_memtable_destroy(memtable);
}

/*
 * Process a single document tuple into local memtable
 */
static void
tp_worker_process_document(
		TpLocalMemtable *memtable,
		TupleTableSlot	*slot,
		int				 attnum,
		Oid				 text_config_oid)
{
	bool		isnull;
	Datum		text_datum;
	text	   *document_text;
	ItemPointer ctid;
	char	   *document_str;
	Datum		tsvector_datum;
	TSVector	tsvector;
	int			i;
	int			doc_length = 0;
	WordEntry  *we;

	/* Get text value */
	text_datum = slot_getattr(slot, attnum, &isnull);
	if (isnull)
		return;

	document_text = DatumGetTextP(text_datum);
	ctid		  = &slot->tts_tid;

	if (!ItemPointerIsValid(ctid))
		return;

	/* Tokenize document */
	document_str   = text_to_cstring(document_text);
	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid,
			ObjectIdGetDatum(text_config_oid),
			PointerGetDatum(document_text));
	tsvector = DatumGetTSVector(tsvector_datum);

	if (tsvector->size == 0)
	{
		pfree(document_str);
		return;
	}

	we = ARRPTR(tsvector);

	/* Add each term to local memtable */
	for (i = 0; i < tsvector->size; i++)
	{
		char *lexeme_start = STRPTR(tsvector) + we[i].pos;
		int	  lexeme_len   = we[i].len;
		int32 frequency;

		if (we[i].haspos)
			frequency = (int32)POSDATALEN(tsvector, &we[i]);
		else
			frequency = 1;

		tp_local_memtable_add_term(
				memtable, lexeme_start, lexeme_len, ctid, frequency);

		doc_length += frequency;
	}

	/* Store document length */
	tp_local_memtable_store_doc_length(memtable, ctid, doc_length);

	pfree(document_str);
}

/*
 * Write segment from local memtable
 * Returns the block number of the segment header, or InvalidBlockNumber
 */
static BlockNumber
tp_write_segment_from_local_memtable(
		TpLocalMemtable		  *memtable,
		Relation			   index,
		TpParallelBuildShared *shared,
		int					   worker_id)
{
	TpSegmentWriter	 writer;
	TpSegmentHeader	 header;
	TpDocMapBuilder *docmap;
	TpLocalPosting **sorted_terms;
	int				 num_terms;
	BlockNumber		 header_block;
	uint32			*string_offsets;
	uint32			 string_pos;
	int				 i;

	/* Skip if nothing to write */
	if (memtable->num_terms == 0 || memtable->num_docs == 0)
		return InvalidBlockNumber;

	/* Build docmap from local memtable */
	docmap = tp_docmap_create();
	tp_local_memtable_foreach_doc(memtable, docmap_add_callback, docmap);
	tp_docmap_finalize(docmap);

	/* Get sorted terms */
	sorted_terms = tp_local_memtable_get_sorted_terms(memtable, &num_terms);
	if (num_terms == 0)
	{
		tp_docmap_destroy(docmap);
		pfree(sorted_terms);
		return InvalidBlockNumber;
	}

	/* Initialize writer with page pool */
	tp_segment_writer_init_with_pool(
			&writer,
			index,
			TpParallelPagePool(shared, worker_id),
			shared->pages_per_worker,
			&shared->pool_next[worker_id]);

	header_block = writer.pages[0];

	/* Initialize header */
	memset(&header, 0, sizeof(TpSegmentHeader));
	header.magic		= TP_SEGMENT_MAGIC;
	header.version		= TP_SEGMENT_FORMAT_VERSION;
	header.created_at	= GetCurrentTimestamp();
	header.num_pages	= 0;
	header.num_terms	= num_terms;
	header.level		= 0;
	header.next_segment = InvalidBlockNumber;
	header.num_docs		= memtable->num_docs;
	header.total_tokens = memtable->total_len;

	header.dictionary_offset = sizeof(TpSegmentHeader);

	/* Write placeholder header */
	tp_segment_writer_write(&writer, &header, sizeof(TpSegmentHeader));

	/* Write dictionary num_terms and string offsets */
	tp_segment_writer_write(&writer, &num_terms, sizeof(uint32));

	/* Calculate and write string offsets */
	string_offsets = palloc0(num_terms * sizeof(uint32));
	string_pos	   = 0;
	for (i = 0; i < num_terms; i++)
	{
		string_offsets[i] = string_pos;
		string_pos += sizeof(uint32) + sorted_terms[i]->term_len +
					  sizeof(uint32);
	}
	tp_segment_writer_write(
			&writer, string_offsets, num_terms * sizeof(uint32));

	/* Write string pool */
	header.strings_offset = writer.current_offset;
	for (i = 0; i < num_terms; i++)
	{
		uint32 length	   = sorted_terms[i]->term_len;
		uint32 dict_offset = i * sizeof(TpDictEntry);

		tp_segment_writer_write(&writer, &length, sizeof(uint32));
		tp_segment_writer_write(&writer, sorted_terms[i]->term, length);
		tp_segment_writer_write(&writer, &dict_offset, sizeof(uint32));
	}

	/* Record entries offset */
	header.entries_offset = writer.current_offset;

	/* Write placeholder dict entries */
	{
		TpDictEntry placeholder;
		memset(&placeholder, 0, sizeof(TpDictEntry));
		for (i = 0; i < num_terms; i++)
			tp_segment_writer_write(
					&writer, &placeholder, sizeof(TpDictEntry));
	}

	/* Write postings - simplified version without block storage for now */
	header.postings_offset = writer.current_offset;
	/* TODO: Implement full block-based posting storage */
	/* For now, we write a minimal segment that can be queried */

	/* Skip index offset */
	header.skip_index_offset = writer.current_offset;

	/* Fieldnorm table */
	header.fieldnorm_offset = writer.current_offset;
	tp_segment_writer_write(
			&writer, docmap->fieldnorms, docmap->num_docs * sizeof(uint8));

	/* CTID pages array */
	header.ctid_pages_offset = writer.current_offset;
	tp_segment_writer_write(
			&writer,
			docmap->ctid_pages,
			docmap->num_docs * sizeof(BlockNumber));

	/* CTID offsets array */
	header.ctid_offsets_offset = writer.current_offset;
	tp_segment_writer_write(
			&writer,
			docmap->ctid_offsets,
			docmap->num_docs * sizeof(OffsetNumber));

	/* Flush and finish writer */
	tp_segment_writer_flush(&writer);
	header.num_pages = writer.pages_allocated;

	/* Write page index */
	header.page_index =
			write_page_index(index, writer.pages, writer.pages_allocated);

	/* Write final header */
	{
		Buffer buf;
		Page   page;

		buf = ReadBuffer(index, header_block);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		memcpy((char *)page + SizeOfPageHeaderData,
			   &header,
			   sizeof(TpSegmentHeader));
		MarkBufferDirty(buf);
		UnlockReleaseBuffer(buf);
	}

	tp_segment_writer_finish(&writer);

	/* Cleanup */
	pfree(string_offsets);
	pfree(sorted_terms);
	tp_docmap_destroy(docmap);

	return header_block;
}

/*
 * Link all worker segment chains into L0
 */
static void
tp_link_all_worker_segments(TpParallelBuildShared *shared, Relation index)
{
	TpWorkerSegmentInfo *worker_info;
	Buffer				 metabuf;
	Page				 metapage;
	TpIndexMetaPage		 metap;
	BlockNumber			 chain_head		= InvalidBlockNumber;
	BlockNumber			 chain_tail		= InvalidBlockNumber;
	int					 total_segments = 0;
	int					 i;

	worker_info = TpParallelWorkerInfo(shared);

	/* Chain all worker segments together */
	for (i = 0; i < shared->worker_count; i++)
	{
		if (worker_info[i].segment_head == InvalidBlockNumber)
			continue;

		if (chain_head == InvalidBlockNumber)
		{
			chain_head = worker_info[i].segment_head;
			chain_tail = worker_info[i].segment_tail;
		}
		else
		{
			/* Link chain_tail to this worker's head */
			Buffer			 tail_buf;
			Page			 tail_page;
			TpSegmentHeader *tail_header;

			tail_buf = ReadBuffer(index, chain_tail);
			LockBuffer(tail_buf, BUFFER_LOCK_EXCLUSIVE);
			tail_page				  = BufferGetPage(tail_buf);
			tail_header				  = (TpSegmentHeader *)((char *)tail_page +
												SizeOfPageHeaderData);
			tail_header->next_segment = worker_info[i].segment_head;
			MarkBufferDirty(tail_buf);
			UnlockReleaseBuffer(tail_buf);

			chain_tail = worker_info[i].segment_tail;
		}

		total_segments += worker_info[i].segment_count;
	}

	/* Update metapage */
	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	metap->level_heads[0]  = chain_head;
	metap->level_counts[0] = total_segments;

	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	elog(DEBUG1,
		 "Linked %d segments from %d workers into L0",
		 total_segments,
		 shared->worker_count);
}

/*
 * Finalize statistics in metapage
 */
static void
tp_finalize_parallel_stats(TpParallelBuildShared *shared, Relation index)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	uint64			total_docs;
	uint64			total_len;

	total_docs = pg_atomic_read_u64(&shared->total_docs);
	total_len  = pg_atomic_read_u64(&shared->total_len);

	metabuf = ReadBuffer(index, 0);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	metap->total_docs = (int32)total_docs;
	metap->total_len  = (int64)total_len;
	/* IDF sum calculated lazily on first query */

	MarkBufferDirty(metabuf);
	UnlockReleaseBuffer(metabuf);

	elog(DEBUG1,
		 "Parallel build complete: %lu docs, %lu total length",
		 (unsigned long)total_docs,
		 (unsigned long)total_len);
}

/*
 * Get a page from the pool (called from segment writer)
 * Falls back to FSM/extend if pool exhausted
 */
BlockNumber
tp_pool_get_page(TpParallelBuildShared *shared, int worker_id, Relation index)
{
	BlockNumber *pool;
	uint32		 idx;

	pool = TpParallelPagePool(shared, worker_id);
	idx	 = pg_atomic_fetch_add_u32(&shared->pool_next[worker_id], 1);

	if (idx < (uint32)shared->pages_per_worker)
		return pool[idx];

	/* Pool exhausted - mark and fall back */
	pg_atomic_write_u32(&shared->pool_exhausted, 1);

	/* Extend relation */
	{
		Buffer		buffer;
		BlockNumber block;

		buffer = ReadBufferExtended(
				index, MAIN_FORKNUM, P_NEW, RBM_ZERO_AND_LOCK, NULL);
		block = BufferGetBlockNumber(buffer);
		PageInit(BufferGetPage(buffer), BLCKSZ, 0);
		MarkBufferDirty(buffer);
		UnlockReleaseBuffer(buffer);

		return block;
	}
}
