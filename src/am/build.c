/*
 * Copyright (c) 2025 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * am/build.c - BM25 index build, insert, and spill operations
 */
#include <postgres.h>

#include <access/tableam.h>
#include <catalog/namespace.h>
#include <commands/progress.h>
#include <executor/spi.h>
#include <math.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/value.h>
#include <storage/bufmgr.h>
#include <tsearch/ts_type.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/regproc.h>
#include <utils/snapmgr.h>

#include "am.h"
#include "constants.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "segment/merge.h"
#include "segment/segment.h"
#include "state/metapage.h"
#include "state/state.h"
#include "types/vector.h"

/* Tapir-specific build phases */
#define TP_PHASE_LOADING 2
#define TP_PHASE_WRITING 3

/* Progress reporting interval (tuples) */
#define TP_PROGRESS_REPORT_INTERVAL 1000

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
 * Auto-spill memtable to disk segment when posting count threshold exceeded.
 * This is called after each document insert to check if spill is needed.
 * The threshold is controlled by pg_textsearch.memtable_spill_threshold GUC.
 */
static void
tp_auto_spill_if_needed(TpLocalIndexState *index_state, Relation index_rel)
{
	BlockNumber		segment_root;
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	TpMemtable	   *memtable;
	int64			total_postings;

	if (!index_state || !index_rel || !index_state->shared)
		return;

	/* Check if posting count threshold is exceeded */
	if (tp_memtable_spill_threshold <= 0)
		return;

	memtable = get_memtable(index_state);
	if (!memtable)
		return;

	total_postings = memtable->total_postings;
	if (total_postings < tp_memtable_spill_threshold)
		return;

	elog(DEBUG1,
		 "Auto-spill triggered: %ld posting entries >= threshold %d",
		 (long)total_postings,
		 tp_memtable_spill_threshold);

	/* Write the segment */
	segment_root = tp_write_segment(index_state, index_rel);

	/* Clear memtable and update metapage if spill succeeded */
	if (segment_root != InvalidBlockNumber)
	{
		tp_clear_memtable(index_state);

		/*
		 * Clear docid pages since data is now in segment. This prevents
		 * recovery from re-indexing documents already persisted in segments,
		 * which would cause duplicate entries and slow recovery.
		 */
		tp_clear_docid_pages(index_rel);

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

		elog(DEBUG1,
			 "Auto-spilled memtable to segment at block %u (L0 count: %u)",
			 segment_root,
			 metap->level_counts[0]);

		/* Check if L0 needs compaction */
		tp_maybe_compact_level(index_rel, 0);
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

/*
 * Helper: Extract options from index relation
 */
static void
tp_build_extract_options(
		Relation index,
		char   **text_config_name,
		Oid		*text_config_oid,
		double	*k1,
		double	*b)
{
	TpOptions *options;

	*text_config_name = NULL;
	*text_config_oid  = InvalidOid;

	/* Extract options from index */
	options = (TpOptions *)index->rd_options;
	if (options)
	{
		if (options->text_config_offset > 0)
		{
			*text_config_name = pstrdup(
					(char *)options + options->text_config_offset);
			/* Convert text config name to OID */
			{
				List *names = list_make1(makeString(*text_config_name));

				*text_config_oid = get_ts_config_oid(names, false);
				list_free(names);
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text_config parameter is required for tapir "
							"indexes"),
					 errhint("Specify text_config when creating the index: "
							 "CREATE INDEX ... USING "
							 "tapir(column) WITH (text_config='english')")));
		}

		*k1 = options->k1;
		*b	= options->b;
	}
	else
	{
		/* No options provided - require text_config */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text_config parameter is required for tapir indexes"),
				 errhint("Specify text_config when creating the index: "
						 "CREATE INDEX ... USING "
						 "tapir(column) WITH (text_config='english')")));
	}
}

/*
 * Helper: Initialize metapage for new index
 */
static void
tp_build_init_metapage(
		Relation index, Oid text_config_oid, double k1, double b)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;

	/* Initialize metapage */
	metabuf = ReadBuffer(index, P_NEW);
	Assert(BufferGetBlockNumber(metabuf) == TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);

	tp_init_metapage(metapage, text_config_oid);
	metap	  = (TpIndexMetaPage)PageGetContents(metapage);
	metap->k1 = k1;
	metap->b  = b;

	MarkBufferDirty(metabuf);

	/* Flush metapage to disk immediately to ensure crash recovery works */
	FlushOneBuffer(metabuf);

	UnlockReleaseBuffer(metabuf);
}

/*
 * Calculate the sum of all IDF values for the index
 */
void
tp_calculate_idf_sum(TpLocalIndexState *index_state)
{
	TpMemtable		  *memtable;
	dshash_table	  *string_table;
	dshash_seq_status  status;
	TpStringHashEntry *entry;
	float8			   idf_sum = 0.0;
	int32			   total_docs;
	int32			   term_count = 0;

	Assert(index_state != NULL);
	Assert(index_state->shared != NULL);

	total_docs = index_state->shared->total_docs;
	if (total_docs == 0)
		return; /* No documents, no IDF to calculate */

	memtable = get_memtable(index_state);
	if (!memtable || memtable->string_hash_handle == DSHASH_HANDLE_INVALID)
		return;

	/* Attach to the string hash table */
	string_table = tp_string_table_attach(
			index_state->dsa, memtable->string_hash_handle);

	/* Iterate through all terms and calculate IDF for each */
	dshash_seq_init(&status, string_table, false); /* shared lock */

	while ((entry = (TpStringHashEntry *)dshash_seq_next(&status)) != NULL)
	{
		if (DsaPointerIsValid(entry->key.posting_list))
		{
			TpPostingList *posting_list =
					dsa_get_address(index_state->dsa, entry->key.posting_list);

			/* Calculate RAW IDF for this term (no epsilon adjustment) */
			double idf_numerator   = (double)(total_docs -
											  posting_list->doc_count + 0.5);
			double idf_denominator = (double)(posting_list->doc_count + 0.5);
			double idf_ratio	   = idf_numerator / idf_denominator;
			double raw_idf		   = log(idf_ratio);

			/* Use raw IDF for sum calculation (including negative values) */
			idf_sum += raw_idf;
			term_count++;
		}
	}

	dshash_seq_term(&status);
	dshash_detach(string_table);

	/* Store the IDF sum in shared state */
	index_state->shared->idf_sum = idf_sum;

	/* Update the term count in memtable */
	memtable->total_terms = term_count;
}

/*
 * Helper: Finalize build and update statistics
 */
static void
tp_build_finalize_and_update_stats(
		Relation		   index,
		TpLocalIndexState *index_state,
		uint64			  *total_docs,
		uint64			  *total_len)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;

	Assert(index_state != NULL);

	/* Calculate IDF sum for average IDF computation */
	tp_calculate_idf_sum(index_state);

	/* Get actual statistics from the shared state */
	*total_docs = index_state->shared->total_docs;
	*total_len	= index_state->shared->total_len;

	/* Update metapage with computed statistics */
	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	metap->total_docs = *total_docs;
	metap->total_len  = *total_len;

	MarkBufferDirty(metabuf);

	/* Flush metapage to disk immediately to ensure crash recovery works */
	FlushOneBuffer(metabuf);

	UnlockReleaseBuffer(metabuf);
}

/*
 * Extract terms and frequencies from a TSVector
 * Returns the document length (sum of all term frequencies)
 */
static int
tp_extract_terms_from_tsvector(
		TSVector tsvector,
		char  ***terms_out,
		int32  **frequencies_out,
		int		*term_count_out)
{
	int		   term_count = tsvector->size;
	char	 **terms;
	int32	  *frequencies;
	int		   doc_length = 0;
	int		   i;
	WordEntry *we;

	*term_count_out = term_count;

	if (term_count == 0)
	{
		*terms_out		 = NULL;
		*frequencies_out = NULL;
		return 0;
	}

	we = ARRPTR(tsvector);

	terms		= palloc(term_count * sizeof(char *));
	frequencies = palloc(term_count * sizeof(int32));

	for (i = 0; i < term_count; i++)
	{
		char *lexeme_start = STRPTR(tsvector) + we[i].pos;
		int	  lexeme_len   = we[i].len;
		char *lexeme;

		/* Always allocate on heap for terms array */
		lexeme = palloc(lexeme_len + 1);
		memcpy(lexeme, lexeme_start, lexeme_len);
		lexeme[lexeme_len] = '\0';

		terms[i] = lexeme;

		/* Get frequency from TSVector - count positions or default to 1 */
		if (we[i].haspos)
			frequencies[i] = (int32)POSDATALEN(tsvector, &we[i]);
		else
			frequencies[i] = 1;

		doc_length += frequencies[i];
	}

	*terms_out		 = terms;
	*frequencies_out = frequencies;

	return doc_length;
}

/*
 * Free memory allocated for terms array
 */
static void
tp_free_terms_array(char **terms, int term_count)
{
	int i;

	if (terms == NULL)
		return;

	for (i = 0; i < term_count; i++)
		pfree(terms[i]);

	pfree(terms);
}

/*
 * Setup table scanning for index build
 * Returns the snapshot (PG18+ only) for later unregistration
 */
static Snapshot
tp_setup_table_scan(
		Relation heap, TableScanDesc *scan_out, TupleTableSlot **slot_out)
{
	Snapshot snapshot = NULL;

#if PG_VERSION_NUM >= 180000
	/* PG18: Must register the snapshot for index builds */
	snapshot = GetTransactionSnapshot();
	if (snapshot)
		snapshot = RegisterSnapshot(snapshot);
	*scan_out = table_beginscan(heap, snapshot, 0, NULL);
#else
	*scan_out = table_beginscan(heap, GetTransactionSnapshot(), 0, NULL);
#endif

	*slot_out = table_slot_create(heap, NULL);

	return snapshot;
}

/*
 * Core document processing: convert text to terms and add to posting lists
 * This is shared between index building and docid recovery.
 *
 * If index_rel is provided, auto-spill will occur when memory limit is
 * exceeded. If index_rel is NULL, no auto-spill occurs (recovery path).
 */
bool
tp_process_document_text(
		text			  *document_text,
		ItemPointer		   ctid,
		Oid				   text_config_oid,
		TpLocalIndexState *index_state,
		Relation		   index_rel,
		int32			  *doc_length_out)
{
	char	*document_str;
	Datum	 tsvector_datum;
	TSVector tsvector;
	char   **terms;
	int32	*frequencies;
	int		 term_count;
	int		 doc_length;

	if (!document_text || !index_state)
		return false;

	document_str = text_to_cstring(document_text);

	/* Validate the TID before processing */
	if (!ItemPointerIsValid(ctid))
	{
		elog(WARNING,
			 "Invalid TID during document processing, skipping document");
		pfree(document_str);
		return false;
	}

	/* Vectorize the document using text configuration */
	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid, /* collation */
			ObjectIdGetDatum(text_config_oid),
			PointerGetDatum(document_text));

	tsvector = DatumGetTSVector(tsvector_datum);

	/* Extract lexemes and frequencies from TSVector */
	doc_length = tp_extract_terms_from_tsvector(
			tsvector, &terms, &frequencies, &term_count);

	if (term_count > 0)
	{
		/*
		 * Acquire exclusive lock for this transaction if not already held.
		 * During index build, we acquire once and hold for the entire build.
		 */
		tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

		/* Add document terms to posting lists */
		tp_add_document_terms(
				index_state, ctid, terms, frequencies, term_count, doc_length);

		/*
		 * Check memory after document completion and auto-spill if needed.
		 * Only spill if index_rel is provided (not during recovery).
		 */
		if (index_rel != NULL)
			tp_auto_spill_if_needed(index_state, index_rel);

		/* Free the terms array and individual lexemes */
		tp_free_terms_array(terms, term_count);
		pfree(frequencies);
	}

	if (doc_length_out)
		*doc_length_out = doc_length;

	pfree(document_str);
	return true;
}

/*
 * Process a single document during index build
 * Returns true if document was processed successfully, false to skip
 */
static bool
tp_process_document(
		TupleTableSlot	  *slot,
		IndexInfo		  *indexInfo,
		Oid				   text_config_oid,
		TpLocalIndexState *index_state,
		Relation		   index,
		uint64			  *total_docs)
{
	bool		isnull;
	Datum		text_datum;
	text	   *document_text;
	ItemPointer ctid;
	int32		doc_length;

	/* Get the text column value (first indexed column) */
	text_datum =
			slot_getattr(slot, indexInfo->ii_IndexAttrNumbers[0], &isnull);

	if (isnull)
		return false; /* Skip NULL documents */

	document_text = DatumGetTextPP(text_datum);

	/* Ensure the slot is materialized to get the TID */
	slot_getallattrs(slot);
	ctid = &slot->tts_tid;

	/* Process the document text using shared helper */
	if (!tp_process_document_text(
				document_text,
				ctid,
				text_config_oid,
				index_state,
				index,
				&doc_length))
	{
		return false;
	}

	/* Store the docid for crash recovery (only during index build) */
	tp_add_docid_to_pages(index, ctid);

	(*total_docs)++;
	return true;
}

/*
 * Build a new Tapir index
 */
IndexBuildResult *
tp_build(Relation heap, Relation index, IndexInfo *indexInfo)
{
	IndexBuildResult  *result;
	char			  *text_config_name = NULL;
	Oid				   text_config_oid	= InvalidOid;
	double			   k1, b;
	TableScanDesc	   scan;
	TupleTableSlot	  *slot;
	Snapshot		   snapshot	  = NULL;
	uint64			   total_docs = 0;
	uint64			   total_len  = 0;
	TpLocalIndexState *index_state;

	/* BM25 index build started */
	elog(NOTICE,
		 "BM25 index build started for relation %s",
		 RelationGetRelationName(index));

	/*
	 * Invalidate docid cache to prevent stale entries from a previous build.
	 * This is critical during VACUUM FULL, which creates a new index file
	 * with different block layout than the old one.
	 */
	tp_invalidate_docid_cache();

	/*
	 * Check for expression indexes - BM25 indexes must be on a direct column
	 * reference, not an expression like lower(content).
	 */
	if (indexInfo->ii_IndexAttrNumbers[0] == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BM25 indexes on expressions are not supported"),
				 errhint("Create the index on a column directly, e.g., "
						 "CREATE INDEX ... USING bm25(content)")));
	}

	/* Report initialization phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE,
			PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE);

	/* Extract options from index */
	tp_build_extract_options(
			index, &text_config_name, &text_config_oid, &k1, &b);

	/* Log configuration being used */
	if (text_config_name)
		elog(NOTICE, "Using text search configuration: %s", text_config_name);
	elog(NOTICE, "Using index options: k1=%.2f, b=%.2f", k1, b);

	/* Initialize metapage */
	tp_build_init_metapage(index, text_config_oid, k1, b);

	/*
	 * Initialize index state in BUILD mode with private DSA.
	 * The private DSA will be destroyed and recreated on each spill,
	 * providing perfect memory reclamation.
	 */
	index_state = tp_create_build_index_state(
			RelationGetRelid(index), RelationGetRelid(heap));

	/* Report loading phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);

	/*
	 * Report estimated tuple count for progress tracking.
	 * Use reltuples for estimate (may be -1 if never analyzed).
	 */
	{
		double reltuples  = heap->rd_rel->reltuples;
		int64  tuples_est = (reltuples > 0) ? (int64)reltuples : 0;

		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_TOTAL, tuples_est);
	}

	/* Prepare to scan table */
	snapshot = tp_setup_table_scan(heap, &scan, &slot);

	/* Process each document in the heap */
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		tp_process_document(
				slot,
				indexInfo,
				text_config_oid,
				index_state,
				index,
				&total_docs);

		/* Report progress every TP_PROGRESS_REPORT_INTERVAL tuples */
		if (total_docs % TP_PROGRESS_REPORT_INTERVAL == 0)
		{
			pgstat_progress_update_param(
					PROGRESS_CREATEIDX_TUPLES_DONE, total_docs);

			/* Allow query cancellation */
			CHECK_FOR_INTERRUPTS();
		}
	}

	/* Report final tuple count */
	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, total_docs);

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);

#if PG_VERSION_NUM >= 180000
	/* Unregister the snapshot (PG18+ only) */
	if (snapshot)
		UnregisterSnapshot(snapshot);
#endif

	/* Report writing phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_WRITING);

	/* Finalize posting lists and update statistics */
	tp_build_finalize_and_update_stats(
			index, index_state, &total_docs, &total_len);

	/* Create index build result */
	result				= (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
	result->heap_tuples = total_docs;
	result->index_tuples = total_docs;

	if (OidIsValid(text_config_oid))
	{
		elog(NOTICE,
			 "BM25 index build completed: " UINT64_FORMAT
			 " documents, avg_length=%.2f, "
			 "text_config='%s' (k1=%.2f, b=%.2f)",
			 total_docs,
			 total_len > 0 ? (float4)(total_len / (double)total_docs) : 0.0,
			 text_config_name ? text_config_name : "unknown",
			 k1,
			 b);
	}
	else
	{
		elog(NOTICE,
			 "BM25 index build completed: " UINT64_FORMAT
			 " documents, avg_length=%.2f "
			 "(text_config=%s, k1=%.2f, b=%.2f)",
			 total_docs,
			 total_len > 0 ? (float4)(total_len / (double)total_docs) : 0.0,
			 text_config_name,
			 k1,
			 b);
	}

	/* Report FSM page reuse statistics */
	tp_report_fsm_stats();

	/*
	 * Final spill: Write any remaining memtable data to disk segment.
	 * This must happen BEFORE destroying the private DSA, otherwise all
	 * build data would be lost.
	 */
	{
		TpMemtable *memtable = get_memtable(index_state);

		if (memtable && memtable->total_postings > 0)
		{
			BlockNumber		segment_root;
			Buffer			metabuf;
			Page			metapage;
			TpIndexMetaPage metap;

			elog(DEBUG1,
				 "BUILD MODE: Final spill of %ld posting entries",
				 (long)memtable->total_postings);

			segment_root = tp_write_segment(index_state, index);

			if (segment_root != InvalidBlockNumber)
			{
				/* Link new segment as L0 chain head */
				metabuf = ReadBuffer(index, 0);
				LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
				metapage = BufferGetPage(metabuf);
				metap	 = (TpIndexMetaPage)PageGetContents(metapage);

				if (metap->level_heads[0] != InvalidBlockNumber)
				{
					/* Point new segment to old chain head */
					Buffer			 seg_buf;
					Page			 seg_page;
					TpSegmentHeader *seg_header;

					seg_buf = ReadBuffer(index, segment_root);
					LockBuffer(seg_buf, BUFFER_LOCK_EXCLUSIVE);
					seg_page   = BufferGetPage(seg_buf);
					seg_header = (TpSegmentHeader *)((char *)seg_page +
													 SizeOfPageHeaderData);
					seg_header->next_segment = metap->level_heads[0];
					MarkBufferDirty(seg_buf);
					UnlockReleaseBuffer(seg_buf);
				}

				metap->level_heads[0] = segment_root;
				metap->level_counts[0]++;
				MarkBufferDirty(metabuf);
				UnlockReleaseBuffer(metabuf);

				elog(DEBUG1,
					 "BUILD MODE: Final segment written at block %u",
					 segment_root);
			}
		}
	}

	/*
	 * Finalize build mode: destroy private DSA and transition to global DSA.
	 * This must be done before returning, otherwise queries will try to use
	 * the private DSA which becomes invalid after the build transaction ends.
	 */
	tp_finalize_build_mode(index_state);

	return result;
}

/*
 * Build an empty Tapir index (for CREATE INDEX without data)
 */
void
tp_buildempty(Relation index)
{
	TpOptions	   *options;
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	char		   *text_config_name = NULL;
	Oid				text_config_oid	 = InvalidOid;

	/* Extract options from index */
	options = (TpOptions *)index->rd_options;
	if (options)
	{
		if (options->text_config_offset > 0)
		{
			text_config_name = pstrdup(
					(char *)options + options->text_config_offset);
			{
				List *names = list_make1(makeString(text_config_name));

				text_config_oid = get_ts_config_oid(names, false);
				list_free(names);
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text_config parameter is required for tapir "
							"indexes"),
					 errhint("Specify text_config when creating the index: "
							 "CREATE INDEX ... USING "
							 "tapir(column) WITH (text_config='english')")));
		}
	}
	else
	{
		/* No options provided - require text_config */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text_config parameter is required for tapir indexes"),
				 errhint("Specify text_config when creating the index: "
						 "CREATE INDEX ... USING "
						 "tapir(column) WITH (text_config='english')")));
	}

	/* Create and initialize the metapage */
	metabuf = ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	Assert(BufferGetBlockNumber(metabuf) == TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	metapage = BufferGetPage(metabuf);
	tp_init_metapage(metapage, text_config_oid);

	/* Set additional parameters after init */
	metap			   = (TpIndexMetaPage)PageGetContents(metapage);
	metap->k1		   = TP_DEFAULT_K1;
	metap->b		   = TP_DEFAULT_B;
	metap->total_docs  = 0;
	metap->total_terms = 0;
	metap->total_len   = 0;

	MarkBufferDirty(metabuf);

	/* Flush metapage to disk immediately to ensure crash recovery works */
	FlushOneBuffer(metabuf);

	UnlockReleaseBuffer(metabuf);
}

/*
 * Insert a tuple into the Tapir index
 */
bool
tp_insert(
		Relation		 index,
		Datum			*values,
		bool			*isnull,
		ItemPointer		 ht_ctid,
		Relation		 heapRel,
		IndexUniqueCheck checkUnique,
		bool			 indexUnchanged,
		IndexInfo		*indexInfo)
{
	text			  *document_text;
	Datum			   vector_datum;
	TpVector		  *tpvec;
	TpVectorEntry	  *vector_entry;
	int32			  *frequencies;
	int				   term_count;
	int				   doc_length = 0;
	int				   i;
	TpLocalIndexState *index_state;

	(void)heapRel;		  /* unused */
	(void)checkUnique;	  /* unused */
	(void)indexUnchanged; /* unused */
	(void)indexInfo;	  /* unused */

	/* Skip NULL documents */
	if (isnull[0])
		return true;

	/* Get index state */
	index_state = tp_get_local_index_state(RelationGetRelid(index));

	/*
	 * Acquire exclusive lock for this transaction if not already held.
	 * This ensures memory consistency on NUMA systems and serializes
	 * write transactions with respect to reads.
	 */
	if (index_state != NULL)
	{
		tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
	}

	/* Extract text from first column */
	document_text = DatumGetTextPP(values[0]);

	/* Vectorize the document */
	{
		char *index_name;
		char *schema_name;
		Oid	  namespace_oid = RelationGetNamespace(index);

		schema_name = get_namespace_name(namespace_oid);
		index_name =
				psprintf("%s.%s", schema_name, RelationGetRelationName(index));

		vector_datum = DirectFunctionCall2(
				to_tpvector,
				PointerGetDatum(document_text),
				CStringGetTextDatum(index_name));

		pfree(index_name);
	}
	tpvec = (TpVector *)DatumGetPointer(vector_datum);

	/* Extract term IDs and frequencies from tpvector */
	term_count = tpvec->entry_count;
	if (term_count > 0)
	{
		char **terms = palloc(term_count * sizeof(char *));

		frequencies = palloc(term_count * sizeof(int32));

		vector_entry = TPVECTOR_ENTRIES_PTR(tpvec);
		for (i = 0; i < term_count; i++)
		{
			char *lexeme;

			/* Always allocate on heap for terms array */
			lexeme = palloc(vector_entry->lexeme_len + 1);
			memcpy(lexeme, vector_entry->lexeme, vector_entry->lexeme_len);
			lexeme[vector_entry->lexeme_len] = '\0';

			/* Store the lexeme string directly in terms array */
			terms[i]	   = lexeme;
			frequencies[i] = vector_entry->frequency;
			doc_length += vector_entry->frequency;

			vector_entry = get_tpvector_next_entry(vector_entry);
		}

		/* Add document terms to posting lists (if shared memory available) */
		if (index_state != NULL)
		{
			/* Validate TID before adding to posting list */
			if (!ItemPointerIsValid(ht_ctid))
				elog(WARNING, "Invalid TID in tp_insert, skipping");
			else
			{
				tp_add_document_terms(
						index_state,
						ht_ctid,
						terms,
						frequencies,
						term_count,
						doc_length);

				/* Auto-spill if memory limit exceeded */
				tp_auto_spill_if_needed(index_state, index);
			}
		}

		/* Free the terms array and individual lexemes */
		for (i = 0; i < term_count; i++)
			pfree(terms[i]);
		pfree(terms);
		pfree(frequencies);
	}

	/* Store the docid for crash recovery */
	tp_add_docid_to_pages(index, ht_ctid);

	/* Recalculate IDF sum after insert */
	if (index_state != NULL)
		tp_calculate_idf_sum(index_state);

	return true;
}
