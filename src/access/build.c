/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * build.c - BM25 index build, insert, and spill operations
 */
#include <postgres.h>

#include <access/generic_xlog.h>
#include <access/tableam.h>
#include <access/xact.h>
#include <access/xlog.h>
#include <catalog/namespace.h>
#include <catalog/storage.h>
#include <commands/progress.h>
#include <executor/spi.h>
#include <math.h>
#include <mb/pg_wchar.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/value.h>
#include <optimizer/optimizer.h>
#include <storage/bufmgr.h>
#include <storage/lmgr.h>
#include <storage/lock.h>
#include <tsearch/ts_type.h>
#include <utils/acl.h>
#include <utils/backend_progress.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/regproc.h>

#include "access/am.h"
#include "access/build_context.h"
#include "access/build_parallel.h"
#include "access/rls.h"
#include "constants.h"
#include "index/compaction_request.h"
#include "index/freepage.h"
#include "index/metapage.h"
#include "index/registry.h"
#include "index/state.h"
#include "memtable/chain_source.h"
#include "memtable/log.h"
#include "memtable/page.h"
#include "segment/compaction.h"
#include "segment/dictionary.h"
#include "segment/docmap.h"
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
 * Activated by the object-access hook after PostgreSQL creates the actual
 * BM25 index catalog object.
 */
typedef struct TpBuildProgress
{
	struct TpBuildProgress *previous;
	int						partition_count;
	uint64					total_docs;
	uint64					total_len;
} TpBuildProgress;

static TpBuildProgress *build_progress = NULL;

typedef struct TpPreparedSpill
{
	TpDataSource	*source;
	TermInfo		*terms;
	uint32			 num_terms;
	TpDocMapBuilder *docmap;
	uint64			 docs_delta;
	uint64			 len_delta;
} TpPreparedSpill;

extern int tp_debug_index_lock_exclusive_waiter_gate;
extern int tp_debug_spill_before_finalize_gate;

static void
tp_debug_gate_spill_before_finalize(Relation index)
{
	LOCKTAG gate_tag;
	int		gate_key = tp_debug_spill_before_finalize_gate;

	if (gate_key <= 0)
		return;

	SET_LOCKTAG_ADVISORY(gate_tag, MyDatabaseId, gate_key, 0, 2);
	ereport(LOG,
			(errmsg("pg_textsearch spill before-finalize gate for "
					"index %u backend %d",
					RelationGetRelid(index),
					MyProcPid)));
	(void)LockAcquire(&gate_tag, ShareLock, true, false);
	(void)LockRelease(&gate_tag, ShareLock, true);
	ereport(LOG,
			(errmsg("pg_textsearch spill passed before-finalize gate for "
					"index %u backend %d",
					RelationGetRelid(index),
					MyProcPid)));
}

static void
tp_debug_gate_spill_threshold_check(TpLocalIndexState *index_state)
{
	LOCKTAG gate_tag;
	int		gate_key = tp_debug_index_lock_exclusive_waiter_gate;

	if (gate_key <= 0)
		return;

	SET_LOCKTAG_ADVISORY(gate_tag, MyDatabaseId, gate_key, 0, 2);
	ereport(LOG,
			(errmsg("pg_textsearch spill threshold check passed for index %u "
					"backend %d",
					index_state->shared->index_oid,
					MyProcPid)));
	(void)LockAcquire(&gate_tag, ShareLock, true, false);
	(void)LockRelease(&gate_tag, ShareLock, true);
}

void
tp_build_progress_begin(void)
{
	MemoryContext	 old_context;
	TpBuildProgress *progress;

	old_context = MemoryContextSwitchTo(TopMemoryContext);
	progress	= palloc0(sizeof(*progress));
	MemoryContextSwitchTo(old_context);

	progress->previous = build_progress;
	build_progress	   = progress;
}

void
tp_build_progress_end(void)
{
	TpBuildProgress *progress = build_progress;
	double			 avg_len  = 0.0;

	if (progress == NULL)
		return;

	build_progress = progress->previous;

	if (progress->total_docs > 0)
		avg_len = (double)progress->total_len / (double)progress->total_docs;

	if (progress->partition_count > 1)
		elog(NOTICE,
			 "BM25 index build completed: " UINT64_FORMAT
			 " documents across %d partitions,"
			 " avg_length=%.2f",
			 progress->total_docs,
			 progress->partition_count,
			 avg_len);
	else
		elog(NOTICE,
			 "BM25 index build completed: " UINT64_FORMAT
			 " documents, avg_length=%.2f",
			 progress->total_docs,
			 avg_len);

	pfree(progress);
}

void
tp_build_progress_abort(void)
{
	TpBuildProgress *progress = build_progress;

	if (progress == NULL)
		return;

	build_progress = progress->previous;
	pfree(progress);
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

static bool
tp_prepare_spill(
		TpLocalIndexState *index_state,
		Relation		   index_rel,
		TpPreparedSpill	  *spill)
{
	memset(spill, 0, sizeof(*spill));

	/*
	 * Open a chain source.  This idempotently re-acquires the
	 * per-index LWLock in SHARED mode, which is a no-op since
	 * the caller holds it EXCLUSIVE; the constructor returns
	 * NULL for an empty chain.
	 */
	spill->source =
			tp_memtable_chain_source_create(index_state, index_rel, NULL, 0);
	if (spill->source == NULL)
		return false;

	spill->docs_delta = (uint64)spill->source->total_docs;
	spill->len_delta  = (uint64)spill->source->total_len;

	tp_memtable_chain_source_extract(
			spill->source,
			CurrentMemoryContext,
			&spill->terms,
			&spill->num_terms,
			&spill->docmap);

	return true;
}

static void
tp_finish_spill(
		TpLocalIndexState *index_state,
		Relation		   index_rel,
		TpPreparedSpill	  *spill,
		BlockNumber		  *out_segment_root,
		uint32			   segment_capacity)
{
	BlockNumber root;

	{
		TpIndexMetaPage metap = tp_get_metapage(index_rel);

		if (metap->level_counts[0] >= segment_capacity)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bm25 segment count limit reached at level 0")));
		pfree(metap);
	}

	root = tp_write_segment(
			index_rel, spill->terms, spill->num_terms, spill->docmap);

	if (out_segment_root != NULL)
		*out_segment_root = root;

	/*
	 * Finalize first, then mark dead - crash-safe ordering.
	 *
	 * We must disconnect the chain (clear metap.head) BEFORE stamping
	 * pages DEAD.  If we crash between mark and finalize, the chain
	 * is still reachable via metap.head but pages are stamped DEAD;
	 * once xmin advances, tp_reclaim_dead_memtable_pages would free
	 * pages still in the live chain, causing corruption on FSM reuse.
	 *
	 * By finalizing first: a crash after finalize but before marking
	 * leaves pages unreachable but un-stamped, so reclaim won't touch
	 * them.  Worst case: pages leak until REINDEX.
	 */
	{
		BlockNumber		  chain_head;
		FullTransactionId horizon;
		TpIndexMetaPage	  metap;

		metap	   = tp_get_metapage(index_rel);
		chain_head = metap->memtable_head_blkno;
		pfree(metap);

		tp_debug_gate_spill_before_finalize(index_rel);
		tp_spill_finalize(
				index_state,
				index_rel,
				root,
				spill->docs_delta,
				spill->len_delta,
				segment_capacity);

		if (tp_debug_panic_after_spill_finalize)
		{
			XLogFlush(GetXLogInsertRecPtr());
			elog(PANIC,
				 "pg_textsearch: debug crash after spill finalize "
				 "(tp_debug_panic_after_spill_finalize)");
		}

		/*
		 * Sample only after the unpublication WAL is inserted.  Every
		 * standby snapshot that can still discover the old chain then has
		 * xmin at or below this reuse-conflict horizon.
		 */
		horizon = ReadNextFullTransactionId();
		if (BlockNumberIsValid(chain_head))
			tp_memtable_mark_chain_dead(index_rel, chain_head, horizon);
	}

	/* Free dictionary + docmap (chain source no longer needed). */
	tp_free_dictionary(spill->terms, spill->num_terms);
	tp_docmap_destroy(spill->docmap);
	tp_source_close(spill->source);

	/*
	 * Reset chain_page_count: the chain is empty after
	 * tp_spill_finalize.  Caller holds LW_EXCLUSIVE so no
	 * concurrent insert can have bumped this since the
	 * pre-spill chain extension was published.
	 */
	pg_atomic_write_u32(&index_state->shared->chain_page_count, 0);
}

bool
tp_do_spill(
		TpLocalIndexState *index_state,
		Relation		   index_rel,
		BlockNumber		  *out_segment_root)
{
	TpPreparedSpill spill;

	if (out_segment_root != NULL)
		*out_segment_root = InvalidBlockNumber;

	/*
	 * Standby is read-only; spill is primary-only.  All durable
	 * state lives on disk; redo applies GenericXLog records and
	 * never invokes spill itself.
	 */
	if (RecoveryInProgress())
		return false;

	if (!tp_prepare_spill(index_state, index_rel, &spill))
		return false;

	tp_finish_spill(
			index_state,
			index_rel,
			&spill,
			out_segment_root,
			tp_max_segments_per_level);

	return true;
}

static void
tp_compact_inline(TpLocalIndexState *index_state, Relation index_rel)
{
	tp_compaction_lock(index_rel);
	PG_TRY();
	{
		(void)tp_compact_step(index_state, index_rel);
	}
	PG_FINALLY();
	{
		if (index_state->lock_held)
			tp_release_index_lock(index_state);
		tp_compaction_unlock(index_rel);
	}
	PG_END_TRY();
}

static void
tp_compact_build_private(TpLocalIndexState *index_state, Relation index_rel)
{
	Assert(index_state->lock_held);
	while (tp_compact_step(index_state, index_rel))
		CHECK_FOR_INTERRUPTS();
}

static void
tp_apply_compaction_policy(
		TpLocalIndexState *index_state, Relation index_rel, bool spilled)
{
	if (!spilled)
		return;

	/*
	 * Parallel VACUUM calls index AM routines while the leader and workers
	 * are in parallel mode, where PostgreSQL forbids assigning an XID.
	 * Publication needs an assigned XID for its standby-safe reclaim stamp,
	 * so leave the durable spill in L0 and let later serial maintenance
	 * compact it.
	 */
	if (IsInParallelMode() || IsParallelWorker())
		return;

	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_COMPACTING);
	switch (tp_index_compaction_mode(index_rel))
	{
	case TP_COMPACTION_INLINE:
		tp_compact_inline(index_state, index_rel);
		break;
	case TP_COMPACTION_BACKGROUND:
		/*
		 * Temporary indexes are unreachable from other sessions, and a
		 * process that cannot dispatch would record a request that is
		 * later discarded.  Both compact inline.
		 */
		if (RelationUsesLocalBuffers(index_rel) ||
			!tp_compaction_dispatch_possible())
			tp_compact_inline(index_state, index_rel);
		else if (tp_compaction_needed(index_rel))
			tp_compaction_request(RelationGetRelid(index_rel));
		break;
	case TP_COMPACTION_MANUAL:
		break;
	}
	pgstat_progress_update_param(
			PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_LOADING);
}

/*
 * Spill memtable to an L0 segment.  Caller passes a minimum
 * chain-page count below which the spill is a no-op — used by
 * VACUUM cleanup and the shutdown hook to avoid producing runt
 * L0 segments on lightly-loaded indexes.  The pre-lock read is
 * a fast bailout; the authoritative check runs after exclusive
 * acquisition because another backend may have spilled meanwhile.
 */
void
tp_spill_memtable_if_needed(
		Relation index, TpLocalIndexState *index_state, uint32 min_pages)
{
	bool spilled = false;

	/* Standby is read-only; spill is primary-only. */
	if (RecoveryInProgress())
		return;

	if (!index_state || !index_state->shared)
		return;

	if (pg_atomic_read_u32(&index_state->shared->chain_page_count) < min_pages)
		return;

	tp_debug_gate_spill_threshold_check(index_state);
	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
	PG_TRY();
	{
		if (pg_atomic_read_u32(&index_state->shared->chain_page_count) >=
			min_pages)
			spilled = tp_do_spill(index_state, index, NULL);
	}
	PG_FINALLY();
	{
		if (index_state->lock_held)
			tp_release_index_lock(index_state);
	}
	PG_END_TRY();

	tp_apply_compaction_policy(index_state, index, spilled);
}

/*
 * Auto-spill the on-disk memtable when the chain grows past the
 * configured page threshold (issue #374).
 *
 * The shared spill helper owns the fast and under-lock threshold
 * checks so every threshold-driven caller has identical race
 * behavior.
 */
static void
tp_auto_spill_if_needed(TpLocalIndexState *index_state, Relation index_rel)
{
	uint32 threshold = (uint32)tp_memtable_pages_threshold;

	if (threshold == 0)
		return; /* auto-spill disabled */

	tp_spill_memtable_if_needed(index_rel, index_state, threshold);
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

	tp_check_level_count_increment(metap, 0);

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
 * Truncate recyclable pages from the end of an index relation.
 *
 * Only a contiguous EOF suffix already carrying TP_FREE_PAGE_MAGIC is safe
 * to remove.  That stamp proves the page was unlinked from every owning
 * structure and completed the appropriate reclaim protocol, including
 * standby conflict WAL when it could still be visible to an old snapshot.
 *
 * Absence from the current published graph is not proof of recyclability:
 * DEAD memtable pages remain protected by dead_fxid, and a crash or handled
 * error can leave unreachable structural pages.  Stop at any DEAD, live,
 * unknown, or orphan page rather than inferring that it is disposable.
 *
 * Caller must hold the per-index LWLock EXCLUSIVE so no allocator can claim
 * a stamped suffix page between inspection and RelationTruncate().
 */
void
tp_truncate_dead_pages(Relation index)
{
	BlockNumber nblocks		= RelationGetNumberOfBlocks(index);
	BlockNumber truncate_to = nblocks;

	while (truncate_to > TP_METAPAGE_BLKNO + 1)
	{
		BlockNumber blk = truncate_to - 1;
		Buffer		buf;
		Page		page;
		bool		recyclable;

		buf = ReadBuffer(index, blk);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page	   = BufferGetPage(buf);
		recyclable = tp_page_is_recyclable(page);
		UnlockReleaseBuffer(buf);

		if (!recyclable)
			break;
		truncate_to = blk;
	}

	if (truncate_to < nblocks)
		RelationTruncate(index, truncate_to);
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
	bool			   spilled;

	/* Replica is read-only; spill is primary-only. Standby's
	 * memtable changes only via WAL redo. */
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("bm25_spill_index() cannot run during recovery")));

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

	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
	PG_TRY();
	{
		/*
		 * Use the out-param so the legacy `bm25_spill_index()` SRF
		 * returns the new L0 segment's block number even when the policy
		 * immediately folds that segment into L1.
		 */
		segment_root = InvalidBlockNumber;
		spilled		 = tp_do_spill(index_state, index_rel, &segment_root);
	}
	PG_FINALLY();
	{
		if (index_state->lock_held)
			tp_release_index_lock(index_state);
	}
	PG_END_TRY();

	tp_apply_compaction_policy(index_state, index_rel, spilled);
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
 * Run one bounded best-effort compaction pass.  Useful after bulk loads or
 * when benchmarking with a compact segment layout.
 */
Datum
tp_force_merge(PG_FUNCTION_ARGS)
{
	text	 *index_name_text = PG_GETARG_TEXT_PP(0);
	char	 *index_name	  = text_to_cstring(index_name_text);
	Oid		  index_oid;
	Relation  index_rel;
	RangeVar *rv;

	/* Replica is read-only; compaction is primary-only. Standby's
	 * segments change only via WAL redo. */
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("bm25_force_merge() cannot run during recovery")));

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

	/* Serialize same-index maintenance before phase-specific LWLocks. */
	{
		TpLocalIndexState *index_state = tp_get_local_index_state(index_oid);
		TpPreparedSpill	   spill;
		bool			   has_memtable;

		if (index_state == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not get index state for "
							"\"%s\"",
							index_name)));

		tp_compaction_lock(index_rel);
		PG_TRY();
		{
			tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
			has_memtable = tp_prepare_spill(index_state, index_rel, &spill);
			if (has_memtable)
				tp_finish_spill(
						index_state, index_rel, &spill, NULL, PG_UINT16_MAX);
			tp_release_index_lock(index_state);

			tp_force_compact(index_state, index_rel);

			tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
			tp_truncate_dead_pages(index_rel);
		}
		PG_FINALLY();
		{
			if (index_state->lock_held)
				tp_release_index_lock(index_state);
			tp_compaction_unlock(index_rel);
		}
		PG_END_TRY();
	}

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
 *
 * Frees the per-chunk `text` wrapper and the tsvector backing before
 * returning, so callers running many chunks in a row (the chunked path
 * in tp_tokenize_text) do not retain ~256 KB + tsvector backing per
 * chunk in CurrentMemoryContext. The output term strings have already
 * been copied into freshly palloc'd buffers by
 * tp_extract_terms_from_tsvector, so they survive the frees.
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
	int		 doc_length;

	chunk_text = cstring_to_text_with_len(chunk, chunk_len);

	tsvector_datum = DirectFunctionCall2Coll(
			to_tsvector_byid,
			InvalidOid,
			ObjectIdGetDatum(text_config_oid),
			PointerGetDatum(chunk_text));
	tsvector = DatumGetTSVector(tsvector_datum);

	doc_length = tp_extract_terms_from_tsvector(
			tsvector, terms_out, frequencies_out, term_count_out);

	pfree(chunk_text);
	pfree(tsvector);

	return doc_length;
}

/*
 * Find a chunk boundary inside the first `target` bytes of `data`.
 *
 * Prefers the byte index just past the last ASCII whitespace at or
 * before `target`. If no whitespace is found, returns the largest
 * UTF-8/multibyte codepoint boundary <= target (and never less than one
 * full character).
 *
 * `data` must be at least `target` bytes long. Returns 1..target.
 */
static int
tp_find_chunk_boundary(const char *data, int target)
{
	int i;
	int pos;

	for (i = target; i > 0; i--)
	{
		char c = data[i - 1];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
			return i;
	}

	pos = 0;
	while (pos < target)
	{
		int mblen = pg_mblen(data + pos);
		if (mblen <= 0)
			mblen = 1;
		if (pos + mblen > target)
			break;
		pos += mblen;
	}
	if (pos == 0)
	{
		/* Single character is larger than target; emit it anyway. */
		pos = pg_mblen(data);
		if (pos <= 0)
			pos = 1;
	}
	return pos;
}

typedef struct TpTermEntry
{
	char *term;
	int32 freq;
} TpTermEntry;

static int
tp_term_entry_cmp(const void *a, const void *b)
{
	const TpTermEntry *ea = (const TpTermEntry *)a;
	const TpTermEntry *eb = (const TpTermEntry *)b;
	return strcmp(ea->term, eb->term);
}

/*
 * Merge accumulated (term, freq) entries by sorting then collapsing
 * adjacent duplicates. Output arrays are palloc'd in the current
 * memory context. Frees `entries` itself; per-entry term strings are
 * either reassigned into the output or pfree'd as duplicates.
 */
static void
tp_merge_term_entries(
		TpTermEntry *entries,
		int			 entry_count,
		char	  ***terms_out,
		int32	   **frequencies_out,
		int			*term_count_out)
{
	int	   out;
	int	   i;
	char **terms;
	int32 *freqs;

	if (entry_count == 0)
	{
		*terms_out		 = NULL;
		*frequencies_out = NULL;
		*term_count_out	 = 0;
		if (entries != NULL)
			pfree(entries);
		return;
	}

	qsort(entries, entry_count, sizeof(TpTermEntry), tp_term_entry_cmp);

	terms = palloc(entry_count * sizeof(char *));
	freqs = palloc(entry_count * sizeof(int32));

	out		   = 0;
	terms[out] = entries[0].term;
	freqs[out] = entries[0].freq;
	for (i = 1; i < entry_count; i++)
	{
		if (strcmp(entries[i].term, terms[out]) == 0)
		{
			freqs[out] += entries[i].freq;
			pfree(entries[i].term);
		}
		else
		{
			out++;
			terms[out] = entries[i].term;
			freqs[out] = entries[i].freq;
		}
	}
	out++;

	pfree(entries);

	*terms_out		 = terms;
	*frequencies_out = freqs;
	*term_count_out	 = out;
}

int
tp_tokenize_text(
		text   *document_text,
		Oid		text_config_oid,
		char ***terms_out,
		int32 **frequencies_out,
		int	   *term_count_out)
{
	const char	*data		= VARDATA_ANY(document_text);
	int			 len		= VARSIZE_ANY_EXHDR(document_text);
	int			 doc_length = 0;
	int			 offset;
	int			 cap;
	int			 used;
	TpTermEntry *acc;

	/*
	 * Single-chunk fast path: pass document_text straight into
	 * to_tsvector_byid. Avoids the cstring_to_text_with_len memcpy that
	 * tp_tokenize_chunk would otherwise do on every small document.
	 */
	if (len <= TP_TSVECTOR_CHUNK_BYTES)
	{
		Datum	 tsvector_datum;
		TSVector tsvector;

		tsvector_datum = DirectFunctionCall2Coll(
				to_tsvector_byid,
				InvalidOid,
				ObjectIdGetDatum(text_config_oid),
				PointerGetDatum(document_text));
		tsvector = DatumGetTSVector(tsvector_datum);
		return tp_extract_terms_from_tsvector(
				tsvector, terms_out, frequencies_out, term_count_out);
	}

	/* Chunked path: accumulate per-chunk (term, freq) into acc[]. */
	cap	 = 1024;
	used = 0;
	acc	 = palloc(cap * sizeof(TpTermEntry));

	offset = 0;
	while (offset < len)
	{
		int	   remaining = len - offset;
		int	   take		 = remaining <= TP_TSVECTOR_CHUNK_BYTES
								 ? remaining
								 : tp_find_chunk_boundary(
								   data + offset, TP_TSVECTOR_CHUNK_BYTES);
		char **chunk_terms;
		int32 *chunk_freqs;
		int	   chunk_term_count;
		int	   i;

		doc_length += tp_tokenize_chunk(
				data + offset,
				take,
				text_config_oid,
				&chunk_terms,
				&chunk_freqs,
				&chunk_term_count);

		if (used + chunk_term_count > cap)
		{
			while (used + chunk_term_count > cap)
				cap *= 2;
			acc = repalloc(acc, cap * sizeof(TpTermEntry));
		}
		for (i = 0; i < chunk_term_count; i++)
		{
			acc[used].term = chunk_terms[i]; /* takes ownership */
			acc[used].freq = chunk_freqs[i];
			used++;
		}
		if (chunk_terms != NULL)
			pfree(chunk_terms);
		if (chunk_freqs != NULL)
			pfree(chunk_freqs);

		offset += take;
	}

	tp_merge_term_entries(
			acc, used, terms_out, frequencies_out, term_count_out);
	return doc_length;
}

/*
 * Core document processing: convert text to terms and add to posting lists.
 * This is shared between CREATE INDEX build (heap scan) and DML inserts
 * (single-tuple aminsert).
 *
 * If index_rel is provided, auto-spill will occur when memory limit is
 * exceeded. If index_rel is NULL, no auto-spill occurs.
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
	char  *document_str;
	char **terms;
	int32 *frequencies;
	int	   term_count;
	int	   doc_length;

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
			document_text, text_config_oid, &terms, &frequencies, &term_count);

	if (index_rel != NULL)
	{
		char	 *index_name = get_rel_name(RelationGetRelid(index_rel));
		TpVector *tpvec;

		tpvec = create_tpvector_from_strings(
				index_name, term_count, (const char **)terms, frequencies);
		pfree(index_name);

		/*
		 * Acquire exclusive lock for this transaction if not already held.
		 * During index build, we acquire once and hold for the entire build.
		 */
		tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

		tp_add_document_terms(
				index_state,
				index_rel,
				ctid,
				(const char *)tpvec,
				(uint32)VARSIZE(tpvec),
				term_count,
				doc_length);

		tp_auto_spill_if_needed(index_state, index_rel);

		pfree(tpvec);
		tp_free_terms_array(terms, term_count);
		pfree(frequencies);
	}
	else if (term_count > 0)
	{
		/*
		 * Legacy recovery path with no Relation: under Phase 4
		 * the rebuild function is short-circuited and never
		 * reaches here, but we keep the cleanup path correct.
		 */
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
	TpBuildContext	  *build_ctx;
	TpLocalIndexState *index_state;
	Relation		   index;
	Oid				   text_config_oid;
	MemoryContext	   per_doc_ctx;
	bool			   is_text_array;
	uint64			   total_docs;
	uint64			   total_len;
	uint64			   tuples_done;
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
			document_text,
			bs->text_config_oid,
			&terms,
			&frequencies,
			&term_count);

	MemoryContextSwitchTo(oldctx);

	tp_build_context_add_document(
			bs->build_ctx, terms, frequencies, term_count, doc_length, ctid);

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
		if (tp_index_compaction_mode(bs->index) == TP_COMPACTION_INLINE)
			tp_compact_build_private(bs->index_state, bs->index);
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

	tp_check_bm25_build_allowed(heap);
	tp_rls_note_bm25_build();

	/* Show "started" for first partition only (suppresses duplicates) */
	if (build_progress == NULL || build_progress->partition_count == 0)
		elog(NOTICE,
			 "BM25 index build started for relation %s",
			 RelationGetRelationName(index));

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
	if (build_progress == NULL || build_progress->partition_count == 0)
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
			if (build_progress == NULL &&
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
			 * first-access bootstrap path), which can race
			 * with concurrent backends touching the same
			 * index and re-creating its registry entry.
			 *
			 * By creating the state here — the same backend
			 * that ran the build — we ensure the registry
			 * entry and local cache are ready before the
			 * CREATE INDEX transaction commits.
			 */
			{
				Buffer			metabuf;
				Page			mpage;
				TpIndexMetaPage metap;

				(void)tp_create_shared_index_state(
						RelationGetRelid(index),
						RelationGetRelid(heap),
						/* reuse_if_exists */ false);

				if (build_progress != NULL)
				{
					metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
					LockBuffer(metabuf, BUFFER_LOCK_SHARE);
					mpage = BufferGetPage(metabuf);
					metap = (TpIndexMetaPage)PageGetContents(mpage);

					build_progress->total_docs += (uint64)metap->total_docs;
					build_progress->total_len += (uint64)metap->total_len;
					build_progress->partition_count++;

					UnlockReleaseBuffer(metabuf);
				}
			}

			return par_result;
		}

		if (build_progress == NULL &&
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
		tp_acquire_index_lock(index_state, LW_EXCLUSIVE);

		/* Budget: maintenance_work_mem (in KB) -> bytes */
		budget = (Size)maintenance_work_mem * 1024L;
		/*
		 * Clamp to the arena's addressable capacity so a large
		 * maintenance_work_mem still spills instead of overrunning
		 * the 4 GiB arena (issue #433).
		 */
		budget	  = tp_arena_clamp_budget(budget);
		build_ctx = tp_build_context_create(budget);

		/* Initialize callback state */
		bs.build_ctx	   = build_ctx;
		bs.index_state	   = index_state;
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
		{
			tp_build_flush_and_link(build_ctx, index);
			if (tp_index_compaction_mode(index) == TP_COMPACTION_INLINE)
			{
				pgstat_progress_update_param(
						PROGRESS_CREATEIDX_SUBPHASE, TP_PHASE_COMPACTING);
				tp_compact_build_private(index_state, index);
			}
		}

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

		/* Create index build result */
		result = (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
		result->heap_tuples	 = reltuples;
		result->index_tuples = total_docs;

		if (build_progress != NULL)
		{
			/* Accumulate stats for aggregated summary */
			build_progress->total_docs += total_docs;
			build_progress->total_len += total_len;
			build_progress->partition_count++;
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
 * chain append, then released before the auto-spill check
 * (which may need LW_EXCLUSIVE).
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

			vector_entry = tpvector_entry_decode_advance(vector_entry, &v);

			lexeme = palloc(v.lexeme_len + 1);
			memcpy(lexeme, v.lexeme, v.lexeme_len);
			lexeme[v.lexeme_len] = '\0';

			terms[i]	   = lexeme;
			frequencies[i] = (int32)v.frequency;
			doc_length += v.frequency;
		}
	}

	/* --- Phase 2: Shared-memory + chain-page work (under lock) --- */
	index_state = tp_get_local_index_state(RelationGetRelid(index));

	if (index_state != NULL)
	{
		/*
		 * Acquire per-index lock in SHARED mode.  Phase 4 does
		 * not require any hash-table initialization here — the
		 * chain pages are extended on demand by
		 * tp_memtable_append() while we hold per-buffer EXCL
		 * on the tail.  Spill holds LW_EXCLUSIVE to fence
		 * inserts during the chain-reset step.
		 */
		tp_acquire_index_lock(index_state, LW_SHARED);

		/* Validate TID before appending to the chain */
		if (!ItemPointerIsValid(ht_ctid))
			elog(WARNING, "Invalid TID in tp_insert, skipping");
		else
		{
			tp_add_document_terms(
					index_state,
					index,
					ht_ctid,
					(const char *)tpvec,
					(uint32)VARSIZE(tpvec),
					term_count,
					doc_length);
		}

		/* Release lock before spill check */
		tp_release_index_lock(index_state);

		/*
		 * Auto-spill check runs outside the lock — may
		 * acquire LW_EXCLUSIVE if a spill is needed.
		 */
		tp_auto_spill_if_needed(index_state, index);
	}
	else if (ItemPointerIsValid(ht_ctid))
	{
		/*
		 * No shared state for this index -- nothing to do.
		 * Memtable v2 (issue #374) writes go through
		 * tp_memtable_append, which needs a registered
		 * shared-state entry to take the per-index LWLock; if
		 * the entry isn't there yet, the backend hasn't
		 * touched the index successfully yet and isn't going
		 * to insert anyway.
		 */
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
