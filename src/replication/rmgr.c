/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * rmgr.c - Custom WAL resource manager for pg_textsearch
 *
 * Implements the rmgr (ID 149) callbacks for streaming-replication
 * and crash-recovery replay of memtable mutations:
 *
 *   - tp_redo_apply_insert_terms: re-applies a doc's term list to
 *     the in-shared-memory memtable on the standby / recovering
 *     primary.
 *   - tp_redo_apply_spill: clears the in-shared-memory memtable so
 *     the standby's view matches the primary post-spill (the
 *     segment data itself replicates via GenericXLog records emitted
 *     by segment.c).
 *   - tp_rmgr_desc / tp_rmgr_identify: pg_waldump support.
 *   - tp_xlog_insert_terms / tp_xlog_spill: emission helpers,
 *     called from the primary's insert and spill paths inside a
 *     critical section.
 *
 * Redo runs in the startup process, which has no transaction
 * state and so cannot use tp_get_local_index_state (that path
 * opens the index relation). Instead the redo helpers look up
 * the per-index shared state directly via tp_registry_lookup
 * and construct a minimal stack-local TpLocalIndexState carrying
 * the {dsa, shared} pair the memtable mutation primitives read.
 */
#include <postgres.h>

#include <access/xlog.h>
#include <access/xlog_internal.h>
#include <access/xloginsert.h>
#include <access/xlogreader.h>
#include <lib/stringinfo.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <storage/lwlock.h>
#include <varatt.h>

#include "index/metapage.h"
#include "index/registry.h"
#include "index/state.h"
#include "memtable/memtable.h"
#include "memtable/stringtable.h"
#include "replication/replication.h"
#include "segment/segment.h"
#include "types/vector.h"

static const RmgrData tp_rmgr_data = {
		.rm_name	 = TP_RMGR_NAME,
		.rm_redo	 = tp_rmgr_redo,
		.rm_desc	 = tp_rmgr_desc,
		.rm_identify = tp_rmgr_identify,
		.rm_startup	 = NULL,
		.rm_cleanup	 = NULL,
		.rm_mask	 = NULL,
		.rm_decode	 = NULL,
};

void
tp_register_rmgr(void)
{
	RegisterCustomRmgr(TP_RMGR_ID, &tp_rmgr_data);
}

/*
 * Apply an INSERT_TERMS record on a standby (or during primary
 * crash recovery): decode the v2 bm25vector payload, find the
 * per-index shared state in the registry, and apply the term
 * list to the in-shared-memory memtable.
 *
 * The startup process running redo cannot use
 * tp_get_local_index_state — that path opens the index relation,
 * which requires IsTransactionState(). Instead we look up the
 * shared state directly via tp_registry_lookup and construct a
 * minimal TpLocalIndexState (just the {dsa, shared} fields the
 * memtable mutation primitives actually read) on the stack.
 *
 * If no registry entry exists for the index yet, skip — the next
 * backend that opens the index will rebuild the memtable from
 * docid pages, picking up everything the missed redo would have
 * applied.
 */
static void
tp_redo_apply_insert_terms(XLogReaderState *record)
{
	char			   *raw = XLogRecGetData(record);
	xl_tp_insert_terms *hdr = (xl_tp_insert_terms *)raw;
	TpVector		   *vec;
	TpSharedIndexState *shared;
	dsa_area		   *dsa;
	TpLocalIndexState	stack_local;
	TpVectorEntry	   *vector_entry;
	char			  **terms		= NULL;
	int32			   *frequencies = NULL;
	int					term_count;
	int					doc_length = 0;
	int					i;

	/*
	 * Validate header length first — reading hdr->vector_size before
	 * confirming the record contains a full header would dereference
	 * past the validated WAL buffer for a degenerate < sizeof(*hdr)
	 * record. Practically unreachable (CRC + single emitter), but
	 * the check is here, so make it actually correct.
	 */
	if (XLogRecGetDataLen(record) < sizeof(*hdr))
		elog(PANIC,
			 "INSERT_TERMS header truncated: got %u, need %zu",
			 XLogRecGetDataLen(record),
			 sizeof(*hdr));
	if (XLogRecGetDataLen(record) < sizeof(*hdr) + hdr->vector_size)
		elog(PANIC,
			 "INSERT_TERMS vector truncated: declared %u, got %u",
			 hdr->vector_size,
			 (uint32)(XLogRecGetDataLen(record) - sizeof(*hdr)));

	vec = (TpVector *)(raw + sizeof(*hdr));
	vec = tpvector_canonicalize(vec);

	term_count = vec->entry_count;
	if (term_count == 0)
		return;

	/*
	 * Find shared state. If no registry entry yet, skip — first
	 * backend access of this index on the standby will rebuild
	 * from docid pages.
	 */
	{
		/*
		 * tp_registry_lookup returns the dsa_pointer cast as a
		 * TpSharedIndexState *; the caller is responsible for
		 * resolving via dsa_get_address.
		 */
		TpSharedIndexState *raw = tp_registry_lookup(hdr->index_oid);
		dsa_pointer			shared_dp;

		if (raw == NULL)
			return;

		shared_dp = (dsa_pointer)(uintptr_t)raw;
		dsa		  = tp_registry_get_dsa();
		if (dsa == NULL)
			return;
		shared = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);
	}

	memset(&stack_local, 0, sizeof(stack_local));
	stack_local.shared		  = shared;
	stack_local.dsa			  = dsa;
	stack_local.is_build_mode = false;
	stack_local.lock_held	  = false;
	stack_local.lock_mode	  = LW_SHARED;

	/* Decode entries into terms[] / frequencies[]. */
	terms		= palloc(term_count * sizeof(char *));
	frequencies = palloc(term_count * sizeof(int32));

	vector_entry = TPVECTOR_ENTRIES_PTR(vec);
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

	{
		TpMemtable *mt;
		bool		need_init;

		mt		  = get_memtable(&stack_local);
		need_init = mt && (mt->string_hash_handle == DSHASH_HANDLE_INVALID ||
						   mt->doc_lengths_handle == DSHASH_HANDLE_INVALID);

		tp_acquire_index_lock(
				&stack_local, need_init ? LW_EXCLUSIVE : LW_SHARED);

		if (need_init)
			tp_ensure_string_table_initialized(&stack_local);

		tp_add_document_terms(
				&stack_local,
				&hdr->ctid,
				terms,
				frequencies,
				term_count,
				doc_length);

		tp_release_index_lock(&stack_local);
	}

	for (i = 0; i < term_count; i++)
		pfree(terms[i]);
	pfree(terms);
	pfree(frequencies);
}

/*
 * Apply a SPILL record on a standby: atomically link the new L0
 * segment into the chain head, clear the in-shared-memory
 * memtable, and (if appropriate) update the previous chain head's
 * next_segment pointer — all while holding the per-index
 * LW_EXCLUSIVE.
 *
 * Doing the chain-link metapage update here (rather than via a
 * separate GenericXLog record) is what closes the duplicate-doc
 * window on the standby: GenericXLog redo only takes buffer
 * locks, so a standby backend could otherwise hold LW_SHARED
 * between chain-link replay (segment becomes queryable) and
 * SPILL replay (memtable cleared) and see each spilled doc twice.
 *
 * If no registry entry yet exists for the index we still apply
 * the buffer changes (the on-disk metapage / segment header must
 * stay in sync with the primary regardless of whether any backend
 * has populated this standby's registry); the memtable clear is
 * skipped because there's no memtable to clear.
 */
static void
tp_redo_apply_spill(XLogReaderState *record)
{
	xl_tp_spill		   *hdr	   = (xl_tp_spill *)XLogRecGetData(record);
	TpSharedIndexState *shared = NULL;
	dsa_area		   *dsa	   = NULL;
	TpLocalIndexState	stack_local;
	XLogRedoAction		action;
	Buffer				metabuf = InvalidBuffer;
	Buffer				segbuf	= InvalidBuffer;
	bool				registry_hit;

	/*
	 * Find shared state (if registered). We need the lock-and-clear
	 * to happen before the buffer modifications become visible to
	 * scanners, so registry lookup happens first.
	 */
	{
		TpSharedIndexState *raw = tp_registry_lookup(hdr->index_oid);

		registry_hit = (raw != NULL);
		if (registry_hit)
		{
			dsa_pointer shared_dp = (dsa_pointer)(uintptr_t)raw;

			dsa = tp_registry_get_dsa();
			if (dsa == NULL)
				registry_hit = false;
			else
				shared = (TpSharedIndexState *)dsa_get_address(dsa, shared_dp);
		}
	}

	if (registry_hit)
	{
		memset(&stack_local, 0, sizeof(stack_local));
		stack_local.shared		  = shared;
		stack_local.dsa			  = dsa;
		stack_local.is_build_mode = false;
		stack_local.lock_held	  = false;
		stack_local.lock_mode	  = LW_SHARED;
		tp_acquire_index_lock(&stack_local, LW_EXCLUSIVE);
	}

	/* Apply chain-link update to the metapage. */
	action = XLogReadBufferForRedo(record, 0, &metabuf);
	if (action == BLK_NEEDS_REDO)
	{
		Page			page  = BufferGetPage(metabuf);
		TpIndexMetaPage metap = (TpIndexMetaPage)PageGetContents(page);

		metap->level_heads[0] = hdr->new_segment_root;
		metap->level_counts[0]++;

		PageSetLSN(page, record->EndRecPtr);
		MarkBufferDirty(metabuf);
	}
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);

	/* Apply next_segment update to the new segment header (if linking). */
	if (XLogRecHasBlockRef(record, 1))
	{
		action = XLogReadBufferForRedo(record, 1, &segbuf);
		if (action == BLK_NEEDS_REDO)
		{
			Page			 page		= BufferGetPage(segbuf);
			TpSegmentHeader *seg_header = (TpSegmentHeader *)PageGetContents(
					page);

			seg_header->next_segment = hdr->prev_chain_head;

			PageSetLSN(page, record->EndRecPtr);
			MarkBufferDirty(segbuf);
		}
		if (BufferIsValid(segbuf))
			UnlockReleaseBuffer(segbuf);
	}

	if (registry_hit)
	{
		tp_clear_memtable(&stack_local);
		tp_release_index_lock(&stack_local);
	}
}

void
tp_rmgr_redo(XLogReaderState *record)
{
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
	case XLOG_TP_INSERT_TERMS:
		tp_redo_apply_insert_terms(record);
		break;
	case XLOG_TP_SPILL:
		tp_redo_apply_spill(record);
		break;
	default:
		elog(PANIC, "tp_rmgr_redo: unknown record type %u", info);
	}
}

void
tp_rmgr_desc(StringInfo buf, XLogReaderState *record)
{
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
	case XLOG_TP_INSERT_TERMS:
	{
		xl_tp_insert_terms *r = (xl_tp_insert_terms *)XLogRecGetData(record);
		appendStringInfo(
				buf,
				"index_oid %u, tid (%u,%u), vec_size %u",
				r->index_oid,
				ItemPointerGetBlockNumber(&r->ctid),
				ItemPointerGetOffsetNumber(&r->ctid),
				r->vector_size);
		break;
	}
	case XLOG_TP_SPILL:
	{
		xl_tp_spill *r = (xl_tp_spill *)XLogRecGetData(record);
		appendStringInfo(
				buf,
				"index_oid %u, new_seg %u, prev_head %u",
				r->index_oid,
				r->new_segment_root,
				r->prev_chain_head);
		break;
	}
	default:
		appendStringInfo(buf, "UNKNOWN info %u", info);
	}
}

const char *
tp_rmgr_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
	case XLOG_TP_INSERT_TERMS:
		return "INSERT_TERMS";
	case XLOG_TP_SPILL:
		return "SPILL";
	}
	return NULL;
}

/*
 * Emission helpers — called from the primary's insert and spill
 * paths inside a critical section.
 */
XLogRecPtr
tp_xlog_insert_terms(Oid index_oid, ItemPointer ctid, const TpVector *vec)
{
	Size vec_size = VARSIZE(vec);
	/*
	 * Designated initializer zeros padding (C99 §6.7.8/21), so the
	 * 2 bytes of compiler-inserted padding between ctid and
	 * vector_size aren't shipped to standbys with stale stack
	 * contents and the WAL bytes are deterministic across runs.
	 */
	xl_tp_insert_terms hdr = {
			.index_oid	 = index_oid,
			.ctid		 = *ctid,
			.vector_size = (uint32)vec_size,
	};

	XLogBeginInsert();
	XLogRegisterData((char *)&hdr, sizeof(hdr));
	XLogRegisterData((char *)vec, vec_size);
	return XLogInsert(TP_RMGR_ID, XLOG_TP_INSERT_TERMS);
}

/*
 * Apply the chain-link metapage update on the primary AND emit a
 * SPILL record carrying enough information for the standby's redo
 * to do the same update atomically with the memtable clear. This
 * function replaces the separate tp_link_l0_chain_head GenericXLog
 * call inside the spill recipes (tp_link_l0_chain_head's standalone
 * use during CREATE INDEX completion still uses GenericXLog).
 *
 * Caller must already hold the per-index LW_EXCLUSIVE.
 */
XLogRecPtr
tp_xlog_spill(Relation index, BlockNumber new_segment_root)
{
	xl_tp_spill		 hdr;
	XLogRecPtr		 lsn;
	Buffer			 metabuf;
	Buffer			 segbuf	  = InvalidBuffer;
	Page			 metapage = NULL;
	Page			 segpage  = NULL;
	TpIndexMetaPage	 metap;
	TpSegmentHeader *seg_header;
	BlockNumber		 prev_chain_head;

	metabuf = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
	metapage = BufferGetPage(metabuf);
	metap	 = (TpIndexMetaPage)PageGetContents(metapage);

	prev_chain_head = metap->level_heads[0];

	if (prev_chain_head != InvalidBlockNumber)
	{
		segbuf = ReadBuffer(index, new_segment_root);
		LockBuffer(segbuf, BUFFER_LOCK_EXCLUSIVE);
		segpage = BufferGetPage(segbuf);
		/* Ensure pd_lower covers the contents (matches the
		 * GenericXLog path tp_link_l0_chain_head used to take). */
		((PageHeader)segpage)->pd_lower = BLCKSZ;
		seg_header				 = (TpSegmentHeader *)PageGetContents(segpage);
		seg_header->next_segment = prev_chain_head;
	}

	metap->level_heads[0] = new_segment_root;
	metap->level_counts[0]++;

	hdr.index_oid		 = RelationGetRelid(index);
	hdr.new_segment_root = new_segment_root;
	hdr.prev_chain_head	 = prev_chain_head;

	XLogBeginInsert();
	XLogRegisterData((char *)&hdr, sizeof(hdr));
	XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
	if (BufferIsValid(segbuf))
		XLogRegisterBuffer(1, segbuf, REGBUF_STANDARD);
	lsn = XLogInsert(TP_RMGR_ID, XLOG_TP_SPILL);

	PageSetLSN(metapage, lsn);
	MarkBufferDirty(metabuf);
	if (BufferIsValid(segbuf))
	{
		PageSetLSN(segpage, lsn);
		MarkBufferDirty(segbuf);
		UnlockReleaseBuffer(segbuf);
	}
	UnlockReleaseBuffer(metabuf);

	return lsn;
}
