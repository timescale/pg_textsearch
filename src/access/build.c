/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build.c - BM25 index build, insert, and spill operations
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <access/tableam.h>
#include <catalog/namespace.h>
#include <catalog/storage.h>
#include <commands/progress.h>
#include <executor/spi.h>
#include <math.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/value.h>
#include <optimizer/optimizer.h>
#include <storage/bufmgr.h>
#include <tsearch/ts_type.h>
#include <utils/acl.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/regproc.h>

#include "access/am.h"
#include "access/build_context.h"
#include "access/build_parallel.h"
#include "constants.h"
#include "index/metapage.h"
#include "index/registry.h"
#include "index/state.h"
#include "memtable/memtable.h"
#include "memtable/posting.h"
#include "memtable/stringtable.h"
#include "segment/io.h"
#include "segment/merge.h"
#include "segment/segment.h"
#include "types/array.h"
#include "types/vector.h"

/*
 * Build progress tracking for partitioned tables.
 *
 * When creating a BM25 index on a partitioned table, tp_build()
 * is called once per partition. Without tracking, each call emits
 * repeated NOTICE messages, producing many lines of noise. This
 * state aggregates statistics across partitions and emits a
 * single summary.
 *
 * Activated by ProcessUtility_hook in mod.c when it detects
 * CREATE INDEX USING bm25.
 */
static struct
{
	bool   active;
	int	   partition_count;
	uint64 total_docs;
	uint64 total_len;
} build_progress;

void
tp_build_progress_begin(void)
{
	memset(&build_progress, 0, sizeof(build_progress));
	build_progress.active = true;
}

void
tp_build_progress_end(void)
{
	double avg_len = 0.0;

	if (!build_progress.active)
		return;

	build_progress.active = false;

	if (build_progress.total_docs > 0)
		avg_len = (double)build_progress.total_len /
				  (double)build_progress.total_docs;

	if (build_progress.partition_count > 1)
		elog(NOTICE,
			 "BM25 index build completed: " UINT64_FORMAT
			 " documents across %d partitions,"
			 " avg_length=%.2f",
			 build_progress.total_docs,
			 build_progress.partition_count,
			 avg_len);
	else
		elog(NOTICE,
			 "BM25 index build completed: " UINT64_FORMAT
			 " documents, avg_length=%.2f",
			 build_progress.total_docs,
			 avg_len);
}

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
	case TP_PHASE_COMPACTING:
		return "compacting segments";
	default:
		return NULL;
	}
}

/*
 * Spill the current index's memtable to a disk segment.
 * Returns true if a segment was written.
 *
 * Caller must already hold LW_EXCLUSIVE on the per-index lock.
 */
static bool
tp_do_spill(TpLocalIndexState *index_state, Relation index_rel)
{
	BlockNumber root;

	root = tp_write_segment(index_state, index_rel);
	if (root == InvalidBlockNumber)
		return false;

	tp_clear_memtable(index_state);
	tp_clear_docid_pages(index_rel);
	tp_link_l0_chain_head(index_rel, root);
	tp_sync_metapage_stats(index_rel, index_state);

	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_COMPACTING);
	tp_maybe_compact_level(index_rel, 0);
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);

	return true;
}

/*
 * Auto-spill memtable when memory limits are exceeded.
 *
 * Checks in order:
 * 1. Legacy memtable_spill_threshold (posting count).
 * 2. Per-index soft limit: memory_limit / 8.
 * 3. Global soft limit: memory_limit / 2
 *    (amortized every ~100 documents).
 *
 * The hard limit (memory_limit itself) is checked separately
 * in tp_check_hard_limit().
 */
static void
tp_auto_spill_if_needed(TpLocalIndexState *index_state, Relation index_rel)
{
	TpMemtable *memtable;
	bool		needs_spill = false;

	if (!index_state || !index_rel || !index_state->shared)
		return;

	memtable = get_memtable(index_state);
	if (!memtable)
		return;

	/*
	 * Check thresholds without the per-index lock.  These reads
	 * are approximate: a concurrent insert may have bumped the
	 * counters since we read them, and a concurrent spill may
	 * have cleared them.  That's fine — a false positive just
	 * triggers an unnecessary lock acquisition (the double-check
	 * under LW_EXCLUSIVE below catches it), and a false negative
	 * means this insert doesn't spill but the next one will.
	 */

	/* Legacy per-index posting count threshold */
	if (tp_memtable_spill_threshold > 0 &&
		pg_atomic_read_u64(&memtable->total_postings) >=
				(uint64)tp_memtable_spill_threshold)
	{
		needs_spill = true;
	}

	if (!needs_spill && tp_memory_limit > 0)
	{
		/* Per-index soft limit: memory_limit / 8 */
		uint64 limit = tp_per_index_limit_bytes();
		uint64 est	 = tp_estimate_memtable_bytes(memtable);

		/* Update global estimate while we have it */
		tp_update_index_estimate(index_state->shared, memtable);

		if (limit > 0 && est > limit)
			needs_spill = true;

		/*
		 * Global soft limit: memory_limit / 2
		 * (amortized every ~100 docs)
		 */
		if (!needs_spill && ++index_state->docs_since_global_check >= 100)
		{
			uint64 g_limit = tp_soft_limit_bytes();
			uint64 g_est;

			index_state->docs_since_global_check = 0;
			g_est = tp_get_estimated_total_bytes();

			if (g_est > g_limit)
			{
				/*
				 * Global memory exceeds the soft limit.
				 * Try to evict the largest memtable.  This
				 * can fail if every candidate is locked by
				 * another backend or has an empty memtable
				 * (stale estimate).
				 */
				if (!tp_evict_largest_memtable(index_state->shared->index_oid))
				{
					elog(WARNING,
						 "pg_textsearch: global soft "
						 "limit exceeded "
						 "(" UINT64_FORMAT " kB / " UINT64_FORMAT
						 " kB) but no "
						 "memtable could be spilled",
						 (uint64)(g_est / 1024),
						 (uint64)(g_limit / 1024));
				}
				return;
			}
		}
	}

	if (!needs_spill)
		return;

	/*
	 * Acquire exclusive lock to spill.  This blocks concurrent
	 * inserters (who hold LW_SHARED) until the spill completes.
	 * The spill itself writes segment pages and updates the
	 * metapage — it does not acquire any other LWLock or
	 * heavyweight lock, so deadlock is not possible.  The
	 * duration is bounded by the memtable size (limited by the
	 * per-index soft limit).
	 */
	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

	/*
	 * Re-check: another backend may have spilled while
	 * we waited for the exclusive lock.
	 */
	memtable = get_memtable(index_state);
	if (memtable)
	{
		bool still_needed = false;

		if (tp_memtable_spill_threshold > 0 &&
			pg_atomic_read_u64(&memtable->total_postings) >=
					(uint64)tp_memtable_spill_threshold)
			still_needed = true;

		if (!still_needed && tp_memory_limit > 0)
		{
			uint64 limit = tp_per_index_limit_bytes();
			uint64 est	 = tp_estimate_memtable_bytes(memtable);
			if (limit > 0 && est > limit)
				still_needed = true;
		}

		if (still_needed)
			tp_do_spill(index_state, index_rel);
	}

	tp_release_index_lock(index_state);
}

/*
 * Hard limit check: fail the current operation if DSA segment
 * memory exceeds memory_limit.  Called from tp_insert before
 * adding terms to the memtable.
 *
 * Raises ERROR if the limit is exceeded; returns normally
 * otherwise.
 */
static void
tp_check_hard_limit(void)
{
	uint64 limit_bytes;
	uint64 dsa_bytes;

	if (tp_memory_limit <= 0)
		return;

	limit_bytes = tp_hard_limit_bytes();

	tp_registry_update_dsa_counter();
	dsa_bytes = tp_registry_get_total_dsa_bytes();

	if (dsa_bytes > limit_bytes)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("pg_textsearch DSA memory "
						"(" UINT64_FORMAT " kB) exceeds "
						"memory_limit "
						"(%d kB)",
						(uint64)(dsa_bytes / 1024),
						tp_memory_limit),
				 errhint("Increase pg_textsearch."
						 "memory_limit or spill "
						 "indexes with "
						 "bm25_spill_index().")));
}

/*
 * Flush build context to a segment and link as L0 chain head.
 * Used during serial CREATE INDEX with arena-based build.
 */
static void
tp_build_flush_and_link(TpBuildContext *ctx, Relation index)
{
	BlockNumber segment_root;

	segment_root = tp_write_segment_from_build_ctx(ctx, index);
	if (segment_root == InvalidBlockNumber)
		return;

	tp_link_l0_chain_head(index, segment_root);
}

/*
 * Link a newly-written segment as the L0 chain head.
 *
 * Reads the metapage, points the new segment's next_segment at the
 * current L0 head, then updates the metapage head and count.
 */
void
tp_link_l0_chain_head(Relation index, BlockNumber segment_root)
{
	Buffer			  metabuf;
	Buffer			  seg_buf = InvalidBuffer;
	GenericXLogState *state;
	Page			  metapage;
	TpIndexMetaPage	  metap;

	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	state	 = GenericXLogStart(index);
	metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	if (metap->level_heads[0] != InvalidBlockNumber)
	{
		Page			 seg_page;
		TpSegmentHeader *seg_header;

		seg_buf = ReadBuffer(index, segment_root);
		LockBuffer(seg_buf, BUFFER_LOCK_EXCLUSIVE);
		seg_page = GenericXLogRegisterBuffer(state, seg_buf, 0);
		/* Ensure pd_lower covers content for GenericXLog */
		((PageHeader)seg_page)->pd_lower = BLCKSZ;
		seg_header = (TpSegmentHeader *)PageGetContents(seg_page);
		seg_header->next_segment = metap->level_heads[0];
	}

	metap->level_heads[0] = segment_root;
	metap->level_counts[0]++;

	GenericXLogFinish(state);
	if (BufferIsValid(seg_buf))
		UnlockReleaseBuffer(seg_buf);
	UnlockReleaseBuffer(metabuf);
}

/*
 * Truncate dead pages from an index relation.
 *
 * Walks all segment chains via the metapage to find the highest
 * block still in use, then truncates everything beyond it.
 * This reclaims pages freed by compaction (which sit below the
 * high-water mark) and unused pool margin from parallel builds.
 */
void
tp_truncate_dead_pages(Relation index)
{
	Buffer			metabuf;
	Page			metapage;
	TpIndexMetaPage metap;
	BlockNumber		max_used = 1; /* at least metapage */
	BlockNumber		nblocks;
	int				level;

	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_SHARE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	for (level = 0; level < TP_MAX_LEVELS; level++)
	{
		BlockNumber seg = metap->level_heads[level];

		while (seg != InvalidBlockNumber)
		{
			TpSegmentReader *reader;
			BlockNumber		*pages;
			uint32			 num_pages;
			uint32			 i;

			num_pages = tp_segment_collect_pages(index, seg, &pages);
			for (i = 0; i < num_pages; i++)
			{
				if (pages[i] + 1 > max_used)
					max_used = pages[i] + 1;
			}
			if (pages)
				pfree(pages);

			reader = tp_segment_open(index, seg);
			seg	   = reader->header->next_segment;
			tp_segment_close(reader);
		}
	}

	UnlockReleaseBuffer(metabuf);

	nblocks = RelationGetNumberOfBlocks(index);
	if (max_used < nblocks)
		RelationTruncate(index, max_used);
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

	/* Check that caller owns the index */
	if (!object_ownercheck(RelationRelationId, index_oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, index_name);

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
		tp_clear_memtable(index_state);
		tp_clear_docid_pages(index_rel);
		tp_link_l0_chain_head(index_rel, segment_root);
		tp_sync_metapage_stats(index_rel, index_state);

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

PG_FUNCTION_INFO_V1(tp_force_merge);

/*
 * SQL-callable: bm25_force_merge(index_name text) → void
 *
 * Force-merge all segments into a single segment, à la Lucene's
 * forceMerge(1).  Useful after bulk loads or when benchmarking
 * with a single-segment layout.
 */
Datum
tp_force_merge(PG_FUNCTION_ARGS)
{
	text	 *index_name_text = PG_GETARG_TEXT_PP(0);
	char	 *index_name	  = text_to_cstring(index_name_text);
	Oid		  index_oid;
	Relation  index_rel;
	RangeVar *rv;

	rv = makeRangeVarFromNameList(stringToQualifiedNameList(index_name, NULL));
	index_oid = RangeVarGetRelid(rv, AccessShareLock, false);

	if (!OidIsValid(index_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" does not exist", index_name)));

	/* Check that caller owns the index */
	if (!object_ownercheck(RelationRelationId, index_oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, index_name);

	index_rel = index_open(index_oid, RowExclusiveLock);
	tp_force_merge_all(index_rel);
	tp_truncate_dead_pages(index_rel);

	index_close(index_rel, RowExclusiveLock);

	PG_RETURN_VOID();
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
				List *names =
						stringToQualifiedNameList(*text_config_name, NULL);

				*text_config_oid = get_ts_config_oid(names, false);
				list_free(names);
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text_config parameter is required for bm25 "
							"indexes"),
					 errhint("Specify text_config when creating the index: "
							 "CREATE INDEX ... USING "
							 "bm25(column) WITH (text_config='english')")));
		}

		*k1 = options->k1;
		*b	= options->b;
	}
	else
	{
		/* No options provided - require text_config */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text_config parameter is required for bm25 indexes"),
				 errhint("Specify text_config when creating the index: "
						 "CREATE INDEX ... USING "
						 "bm25(column) WITH (text_config='english')")));
	}
}

/*
 * Helper: Initialize metapage for new index
 */
static void
tp_build_init_metapage(
		Relation index, Oid text_config_oid, double k1, double b)
{
	Buffer			  metabuf;
	GenericXLogState *state;
	Page			  metapage;
	TpIndexMetaPage	  metap;

	/* Initialize metapage */
	metabuf = ReadBuffer(index, P_NEW);
	Assert(BufferGetBlockNumber(metabuf) == TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	state = GenericXLogStart(index);
	metapage =
			GenericXLogRegisterBuffer(state, metabuf, GENERIC_XLOG_FULL_IMAGE);

	tp_init_metapage(metapage, text_config_oid);
	metap	  = (TpIndexMetaPage)PageGetContents(metapage);
	metap->k1 = k1;
	metap->b  = b;

	GenericXLogFinish(state);
	UnlockReleaseBuffer(metabuf);
}

/*
 * Extract terms and frequencies from a TSVector
 * Returns the document length (sum of all term frequencies)
 */
int
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
 * Maximum input bytes per to_tsvector call. Postgres's tsvector caps the
 * lexeme dictionary at 1 MB (MAXSTRPOS). 256 KB leaves comfortable
 * headroom for stemming/case-fold expansion.
 */
#define TP_TSVECTOR_CHUNK_BYTES (256 * 1024)

/*
 * Tokenize a single chunk via to_tsvector_byid and extract terms.
 * Caller owns the returned arrays. doc_length is returned.
 */
static int
tp_tokenize_chunk(
		const char *chunk,
		int			chunk_len,
		Oid			text_config_oid,
		char	 ***terms_out,
		int32	  **frequencies_out,
		int		   *term_count_out)
{
	text	*chunk_text;
	Datum	 tsvector_datum;
	TSVector tsvector;

	chunk_text = cstring_to_text_with_len(chunk, chunk_len);

	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid,
			ObjectIdGetDatum(text_config_oid),
			PointerGetDatum(chunk_text));
	tsvector = DatumGetTSVector(tsvector_datum);

	return tp_extract_terms_from_tsvector(
			tsvector, terms_out, frequencies_out, term_count_out);
}

int
tp_tokenize_text(
		text   *document_text,
		Oid		text_config_oid,
		char ***terms_out,
		int32 **frequencies_out,
		int	   *term_count_out)
{
	const char *data = VARDATA_ANY(document_text);
	int			len	 = VARSIZE_ANY_EXHDR(document_text);

	/* Single-chunk fast path */
	return tp_tokenize_chunk(
			data, len, text_config_oid,
			terms_out, frequencies_out, term_count_out);
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

	/* Tokenize document (chunks oversized inputs to fit tsvector cap) */
	doc_length = tp_tokenize_text(
			document_text, text_config_oid,
			&terms, &frequencies, &term_count);

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
 * Callback state for table_index_build_scan during serial builds.
 */
typedef struct TpBuildCallbackState
{
	TpBuildContext *build_ctx;
	Relation		index;
	Oid				text_config_oid;
	MemoryContext	per_doc_ctx;
	bool			is_text_array;
	uint64			total_docs;
	uint64			total_len;
	uint64			tuples_done;
} TpBuildCallbackState;

/*
 * Per-tuple callback for table_index_build_scan.
 *
 * Receives pre-evaluated index expression values from the scan,
 * tokenizes the document text, and adds it to the build context.
 */
static void
tp_build_callback(
		Relation	index,
		ItemPointer ctid,
		Datum	   *values,
		bool	   *isnull,
		bool		tupleIsAlive,
		void	   *state)
{
	TpBuildCallbackState *bs = (TpBuildCallbackState *)state;
	text				 *document_text;
	char				**terms;
	int32				 *frequencies;
	int					  term_count;
	int					  doc_length;
	MemoryContext		  oldctx;

	/* Suppress unused parameter warnings for callback signature */
	(void)index;
	(void)tupleIsAlive;

	if (isnull[0])
		return;

	if (!ItemPointerIsValid(ctid))
		return;

	/*
	 * Tokenize in temporary context to prevent
	 * to_tsvector_byid memory from accumulating.
	 * Detoasting also happens here so the copy is freed
	 * by MemoryContextReset below.
	 */
	oldctx = MemoryContextSwitchTo(bs->per_doc_ctx);

	if (bs->is_text_array)
		document_text = tp_flatten_text_array(values[0]);
	else
		document_text = DatumGetTextPP(values[0]);

	doc_length = tp_tokenize_text(
			document_text, bs->text_config_oid,
			&terms, &frequencies, &term_count);

	MemoryContextSwitchTo(oldctx);

	if (term_count > 0)
	{
		tp_build_context_add_document(
				bs->build_ctx,
				terms,
				frequencies,
				term_count,
				doc_length,
				ctid);
	}

	/* Reset per-doc context (frees tsvector, terms) */
	MemoryContextReset(bs->per_doc_ctx);

	/* Budget-based flush */
	if (tp_build_context_should_flush(bs->build_ctx))
	{
		bs->total_docs += bs->build_ctx->num_docs;
		bs->total_len += bs->build_ctx->total_len;

		tp_build_flush_and_link(bs->build_ctx, bs->index);
		tp_build_context_reset(bs->build_ctx);

		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_COMPACTING);
		tp_maybe_compact_level(bs->index, 0);
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);
	}

	bs->tuples_done++;
	if (bs->tuples_done % TP_PROGRESS_REPORT_INTERVAL == 0)
	{
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_DONE, bs->tuples_done);
		CHECK_FOR_INTERRUPTS();
	}
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
	uint64			   total_docs = 0;
	uint64			   total_len  = 0;
	TpLocalIndexState *index_state;
	bool			   is_text_array;

	/* Show "started" for first partition only (suppresses duplicates) */
	if (!build_progress.active || build_progress.partition_count == 0)
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
	 * Determine if the indexed column is a text array type.
	 * If so, we flatten array elements into a single text value
	 * before tokenization. Expression indexes (attnum == 0)
	 * are never text arrays.
	 */
	{
		AttrNumber attnum = indexInfo->ii_IndexAttrNumbers[0];

		if (attnum > 0)
		{
			Oid atttype = TupleDescAttr(RelationGetDescr(heap), attnum - 1)
								  ->atttypid;
			is_text_array = tp_is_text_array_type(atttype);
		}
		else
			is_text_array = false;
	}

	/* Report initialization phase */
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE,
			PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE);

	/* Extract options from index */
	tp_build_extract_options(
			index, &text_config_name, &text_config_oid, &k1, &b);

	/* Log configuration (only for first partition when active) */
	if (!build_progress.active || build_progress.partition_count == 0)
	{
		if (text_config_name)
			elog(NOTICE,
				 "Using text search configuration: %s",
				 text_config_name);
		elog(NOTICE, "Using index options: k1=%.2f, b=%.2f", k1, b);
	}

	/* Initialize metapage */
	tp_build_init_metapage(index, text_config_oid, k1, b);

	/*
	 * Check memory limits before starting build.
	 * The post-build transition allocates a runtime memtable
	 * in the global DSA, so try to free space first.
	 *
	 * Soft limit: try to evict if estimated usage is high.
	 * Hard limit: fail if DSA reservation exceeds the cap.
	 */
	if (tp_memory_limit > 0)
	{
		uint64 soft = tp_soft_limit_bytes();

		if (tp_get_estimated_total_bytes() > soft)
			tp_evict_largest_memtable(InvalidOid);

		{
			uint64 limit = tp_hard_limit_bytes();
			uint64 dsa_bytes;

			tp_registry_update_dsa_counter();
			dsa_bytes = tp_registry_get_total_dsa_bytes();

			if (dsa_bytes > limit)
			{
				tp_evict_largest_memtable(InvalidOid);

				/* Re-check after eviction attempt */
				tp_registry_update_dsa_counter();
				dsa_bytes = tp_registry_get_total_dsa_bytes();

				if (dsa_bytes > limit)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("pg_textsearch DSA "
									"memory (" UINT64_FORMAT " kB) exceeds "
									"memory_limit "
									"(%d kB), cannot "
									"start index build",
									(uint64)(dsa_bytes / 1024),
									tp_memory_limit),
							 errhint("Increase "
									 "pg_textsearch."
									 "memory_limit or "
									 "spill indexes with "
									 "bm25_spill_index().")));
			}
		}
	}

	/*
	 * Check if parallel build is possible and beneficial.
	 *
	 * Postgres has already called plan_create_index_workers() and stored
	 * the result in indexInfo->ii_ParallelWorkers. We use that value
	 * directly to avoid redundant planning work and ensure consistency.
	 *
	 * We add our own minimum tuple threshold (100K) because for smaller
	 * tables, the parallel coordination overhead exceeds the benefit.
	 */
	{
		int	   nworkers	 = indexInfo->ii_ParallelWorkers;
		double reltuples = heap->rd_rel->reltuples;

		/*
		 * Only consider parallel build for tables with 100K+ estimated rows.
		 * For smaller tables, the parallel coordination overhead exceeds
		 * the benefit.
		 *
		 * If reltuples is -1 (table never analyzed), estimate from page count.
		 * We use a conservative estimate of 50 tuples per 8KB page, which
		 * assumes ~160 bytes per row (reasonable for text search workloads).
		 */
#define TP_MIN_PARALLEL_TUPLES		100000
#define TP_TUPLES_PER_PAGE_ESTIMATE 50

		if (reltuples < 0)
		{
			BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
			reltuples = (double)nblocks * TP_TUPLES_PER_PAGE_ESTIMATE;
		}

		/*
		 * Thresholds for warning about suboptimal parallelism.
		 * These are conservative - we only warn when users could see
		 * significant (>2x) speedup from more parallelism.
		 */
#define TP_WARN_NO_PARALLEL_TUPLES 1000000 /* 1M tuples */
#define TP_WARN_FEW_WORKERS_TUPLES 5000000 /* 5M tuples */
#define TP_WARN_FEW_WORKERS_MIN	   2	   /* suggest more if <= this */

		if (nworkers > 0 && reltuples >= TP_MIN_PARALLEL_TUPLES &&
			!indexInfo->ii_Concurrent)
		{
			IndexBuildResult *par_result;

			/*
			 * Warn if table is very large but parallelism is limited.
			 * Suppress during partitioned builds to reduce noise.
			 */
			if (!build_progress.active &&
				reltuples >= TP_WARN_FEW_WORKERS_TUPLES &&
				nworkers <= TP_WARN_FEW_WORKERS_MIN)
			{
				elog(NOTICE,
					 "Large table (%.0f tuples) with only %d parallel "
					 "workers. "
					 "Consider increasing "
					 "max_parallel_maintenance_workers "
					 "and "
					 "maintenance_work_mem (need 32MB per worker) "
					 "for faster builds.",
					 reltuples,
					 nworkers);
			}

			par_result = tp_build_parallel(
					heap,
					index,
					indexInfo,
					text_config_oid,
					k1,
					b,
					is_text_array,
					nworkers);

			/*
			 * Create shared index state for runtime queries.
			 *
			 * The parallel build writes segments and updates
			 * the metapage, but does not create the in-memory
			 * shared state that INSERT and SELECT need.
			 * Without this, the first post-build access falls
			 * through to tp_rebuild_index_from_disk() (the
			 * crash-recovery path), which is fragile: a
			 * concurrent backend can race to recreate the
			 * state, leaving the inserting backend's memtable
			 * invisible to scans.
			 *
			 * By creating the state here — the same backend
			 * that ran the build — we ensure the registry
			 * entry and local cache are ready before the
			 * CREATE INDEX transaction commits.
			 */
			{
				TpLocalIndexState *pstate;
				Buffer			   metabuf;
				Page			   mpage;
				TpIndexMetaPage	   metap;

				pstate = tp_create_shared_index_state(
						RelationGetRelid(index), RelationGetRelid(heap));

				metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
				LockBuffer(metabuf, BUFFER_LOCK_SHARE);
				mpage = BufferGetPage(metabuf);
				metap = (TpIndexMetaPage)PageGetContents(mpage);

				pg_atomic_write_u32(
						&pstate->shared->total_docs, metap->total_docs);
				pg_atomic_write_u64(
						&pstate->shared->total_len, metap->total_len);

				if (build_progress.active)
				{
					build_progress.total_docs += (uint64)metap->total_docs;
					build_progress.total_len += (uint64)metap->total_len;
					build_progress.partition_count++;
				}

				UnlockReleaseBuffer(metabuf);
			}

			return par_result;
		}

		if (!build_progress.active &&
			reltuples >= TP_WARN_NO_PARALLEL_TUPLES && nworkers == 0)
		{
			/*
			 * Large table but no parallel workers available.
			 * This is likely due to
			 * max_parallel_maintenance_workers = 0.
			 */
			elog(NOTICE,
				 "Large table (%.0f tuples) but parallel build "
				 "disabled. "
				 "Set max_parallel_maintenance_workers > 0 and "
				 "ensure "
				 "maintenance_work_mem >= 64MB for faster builds.",
				 reltuples);
		}
	}

	/*
	 * Serial build using arena-based build context.
	 *
	 * Uses table_index_build_scan() which automatically evaluates
	 * index expressions and partial-index predicates, passing
	 * pre-computed values to our callback.
	 * maintenance_work_mem controls the per-batch memory budget.
	 */
	{
		TpBuildCallbackState bs;
		TpBuildContext		*build_ctx;
		Size				 budget;
		double				 reltuples;

		/*
		 * Still create build index state for:
		 * - Per-index LWLock infrastructure
		 * - Post-build transition to runtime mode
		 * - Shared state initialization for runtime queries
		 */
		index_state = tp_create_build_index_state(
				RelationGetRelid(index), RelationGetRelid(heap));

		/* Budget: maintenance_work_mem (in KB) -> bytes */
		budget	  = (Size)maintenance_work_mem * 1024L;
		build_ctx = tp_build_context_create(budget);

		/* Initialize callback state */
		bs.build_ctx	   = build_ctx;
		bs.index		   = index;
		bs.text_config_oid = text_config_oid;
		bs.is_text_array   = is_text_array;
		bs.per_doc_ctx	   = AllocSetContextCreate(
				CurrentMemoryContext,
				"build per-doc temp",
				ALLOCSET_DEFAULT_SIZES);
		bs.total_docs  = 0;
		bs.total_len   = 0;
		bs.tuples_done = 0;

		/* Report loading phase */
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);

		{
			double rt		  = heap->rd_rel->reltuples;
			int64  tuples_est = (rt > 0) ? (int64)rt : 0;

			pgstat_progress_update_param(
					PROGRESS_CREATEIDX_TUPLES_TOTAL, tuples_est);
		}

		/* Scan table with expression evaluation */
		reltuples = table_index_build_scan(
				heap,
				index,
				indexInfo,
				true,
				false,
				tp_build_callback,
				&bs,
				NULL);

		/* Accumulate final batch stats */
		total_docs = bs.total_docs + build_ctx->num_docs;
		total_len  = bs.total_len + build_ctx->total_len;

		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_TUPLES_DONE, bs.tuples_done);

		/* Report writing phase */
		pgstat_progress_update_param(
				PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_WRITING);

		/* Write final segment if data remains */
		if (build_ctx->num_docs > 0)
			tp_build_flush_and_link(build_ctx, index);

		/* Update metapage with corpus statistics */
		{
			Buffer			  metabuf;
			GenericXLogState *state;
			Page			  metapage;
			TpIndexMetaPage	  metap;

			metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
			LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

			state	 = GenericXLogStart(index);
			metapage = GenericXLogRegisterBuffer(state, metabuf, 0);
			metap	 = (TpIndexMetaPage)PageGetContents(metapage);

			metap->total_docs = total_docs;
			metap->total_len  = total_len;

			GenericXLogFinish(state);
			UnlockReleaseBuffer(metabuf);
		}

		/* Update shared state for runtime queries */
		pg_atomic_write_u32(&index_state->shared->total_docs, total_docs);
		pg_atomic_write_u64(&index_state->shared->total_len, total_len);

		/* Create index build result */
		result = (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
		result->heap_tuples	 = reltuples;
		result->index_tuples = total_docs;

		if (build_progress.active)
		{
			/* Accumulate stats for aggregated summary */
			build_progress.total_docs += total_docs;
			build_progress.total_len += total_len;
			build_progress.partition_count++;
		}
		else
		{
			elog(NOTICE,
				 "BM25 index build completed: " UINT64_FORMAT
				 " documents, avg_length=%.2f",
				 total_docs,
				 total_docs > 0 ? (float4)(total_len / (double)total_docs)
								: 0.0);
		}

		/*
		 * Release the per-index lock before finalizing.
		 * Critical for partitioned tables to avoid hitting
		 * MAX_SIMUL_LWLOCKS limit.
		 */
		tp_release_index_lock(index_state);

		/*
		 * Finalize build mode: destroy private DSA and
		 * transition to global DSA for runtime operation.
		 */
		tp_finalize_build_mode(index_state);

		/* Cleanup */
		tp_build_context_destroy(build_ctx);
		MemoryContextDelete(bs.per_doc_ctx);
	}

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
				List *names =
						stringToQualifiedNameList(text_config_name, NULL);

				text_config_oid = get_ts_config_oid(names, false);
				list_free(names);
			}
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("text_config parameter is required for bm25 "
							"indexes"),
					 errhint("Specify text_config when creating the index: "
							 "CREATE INDEX ... USING "
							 "bm25(column) WITH (text_config='english')")));
		}
	}
	else
	{
		/* No options provided - require text_config */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("text_config parameter is required for bm25 indexes"),
				 errhint("Specify text_config when creating the index: "
						 "CREATE INDEX ... USING "
						 "bm25(column) WITH (text_config='english')")));
	}

	/* Create and initialize the metapage */
	metabuf = ReadBufferExtended(index, INIT_FORKNUM, P_NEW, RBM_NORMAL, NULL);
	Assert(BufferGetBlockNumber(metabuf) == TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	{
		GenericXLogState *state;

		state	 = GenericXLogStart(index);
		metapage = GenericXLogRegisterBuffer(
				state, metabuf, GENERIC_XLOG_FULL_IMAGE);

		tp_init_metapage(metapage, text_config_oid);

		/* Set additional parameters after init */
		metap	  = (TpIndexMetaPage)PageGetContents(metapage);
		metap->k1 = TP_DEFAULT_K1;
		metap->b  = TP_DEFAULT_B;

		GenericXLogFinish(state);
	}

	UnlockReleaseBuffer(metabuf);
}

/*
 * Insert a tuple into the Tapir index.
 *
 * Tokenization happens before any lock is acquired so that
 * CPU-intensive text processing does not serialize inserts.
 * The per-index lock is held as LW_SHARED for the memtable
 * write and docid-page update, then released before the
 * auto-spill check (which may need LW_EXCLUSIVE).
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
	int32			  *frequencies = NULL;
	int				   term_count;
	int				   doc_length = 0;
	int				   i;
	TpLocalIndexState *index_state;
	char			 **terms = NULL;

	(void)checkUnique;	  /* unused */
	(void)indexUnchanged; /* unused */

	/* Skip NULL documents */
	if (isnull[0])
		return true;

	/* --- Phase 1: Tokenize (no lock held) --- */
	{
		AttrNumber attnum = indexInfo->ii_IndexAttrNumbers[0];
		Oid		   atttype =
				TupleDescAttr(RelationGetDescr(heapRel), attnum - 1)->atttypid;

		if (tp_is_text_array_type(atttype))
			document_text = tp_flatten_text_array(values[0]);
		else
			document_text = DatumGetTextPP(values[0]);
	}
	{
		char *index_name;
		char *schema_name;
		Oid	  namespace_oid = RelationGetNamespace(index);

		schema_name = get_namespace_name(namespace_oid);
		index_name	= quote_qualified_identifier(
				 schema_name, RelationGetRelationName(index));

		vector_datum = DirectFunctionCall2(
				to_tpvector,
				PointerGetDatum(document_text),
				CStringGetTextDatum(index_name));

		pfree(index_name);
		pfree(schema_name);
	}
	tpvec = (TpVector *)DatumGetPointer(vector_datum);

	/* Extract terms and frequencies */
	term_count = tpvec->entry_count;
	if (term_count > 0)
	{
		terms		= palloc(term_count * sizeof(char *));
		frequencies = palloc(term_count * sizeof(int32));

		vector_entry = TPVECTOR_ENTRIES_PTR(tpvec);
		for (i = 0; i < term_count; i++)
		{
			TpVectorEntryView v;
			char			 *lexeme;

			tpvector_entry_decode(vector_entry, &v);

			lexeme = palloc(v.lexeme_len + 1);
			memcpy(lexeme, v.lexeme, v.lexeme_len);
			lexeme[v.lexeme_len] = '\0';

			terms[i]	   = lexeme;
			frequencies[i] = (int32)v.frequency;
			doc_length += v.frequency;

			vector_entry = get_tpvector_next_entry(vector_entry);
		}
	}

	/* --- Phase 2: Shared-memory work (under lock) --- */
	index_state = tp_get_local_index_state(RelationGetRelid(index));

	if (index_state != NULL && term_count > 0)
	{
		/*
		 * Hard limit check before acquiring the per-index lock.
		 * This is a simple atomic read of the global DSA counter
		 * — it does not flush or evict any memtable.  If over
		 * the hard limit, we ERROR out before touching any
		 * shared state.
		 */
		tp_check_hard_limit();

		/*
		 * Acquire per-index lock. Normally LW_SHARED so
		 * multiple inserters run concurrently.
		 *
		 * Cold path: if the memtable hash tables are not
		 * yet initialized, acquire LW_EXCLUSIVE and init.
		 * We stay exclusive for the cold-path insert to
		 * prevent a concurrent spill from clearing the
		 * tables between init and use.
		 *
		 * The lockless pre-check is an optimization to
		 * avoid exclusive on the hot path. If a concurrent
		 * spill invalidates between the check and lock
		 * acquire, we detect it under the lock and retry.
		 */
		for (;;)
		{
			TpMemtable *mt		  = get_memtable(index_state);
			bool		need_init = mt &&
							 (mt->string_hash_handle ==
									  DSHASH_HANDLE_INVALID ||
							  mt->doc_lengths_handle == DSHASH_HANDLE_INVALID);

			tp_acquire_index_lock(
					index_state, need_init ? LW_EXCLUSIVE : LW_SHARED);

			if (need_init)
			{
				tp_ensure_string_table_initialized(index_state);
				break; /* Hold exclusive for insert */
			}

			/*
			 * Re-check under shared lock: a spill may
			 * have cleared the tables after our lockless
			 * read but before we acquired shared.
			 */
			mt = get_memtable(index_state);
			if (mt && (mt->string_hash_handle == DSHASH_HANDLE_INVALID ||
					   mt->doc_lengths_handle == DSHASH_HANDLE_INVALID))
			{
				/* Stale read — retry with exclusive */
				tp_release_index_lock(index_state);
				continue;
			}
			break;
		}

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

			/*
			 * Docid pages under LW_SHARED — spill clears
			 * these under LW_EXCLUSIVE, so they must be
			 * written while we hold shared.
			 */
			tp_add_docid_to_pages(index, ht_ctid);
		}

		/* Release lock before spill check */
		tp_release_index_lock(index_state);

		/*
		 * Auto-spill check runs outside the lock — may
		 * acquire LW_EXCLUSIVE if a spill is needed.
		 */
		tp_auto_spill_if_needed(index_state, index);
	}
	else if (term_count > 0 && ItemPointerIsValid(ht_ctid))
	{
		/* No shared state but valid doc — record TID */
		tp_add_docid_to_pages(index, ht_ctid);
	}

	/* Free the terms array and individual lexemes */
	if (terms)
	{
		for (i = 0; i < term_count; i++)
			pfree(terms[i]);
		pfree(terms);
		pfree(frequencies);
	}

	return true;
}
