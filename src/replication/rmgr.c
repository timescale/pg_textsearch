/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * rmgr.c - Custom WAL resource manager for pg_textsearch
 *
 * Implements the rmgr (ID 145) callbacks for streaming-replication
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
#include <storage/lwlock.h>
#include <varatt.h>

#include "index/registry.h"
#include "index/state.h"
#include "memtable/memtable.h"
#include "memtable/stringtable.h"
#include "replication/replication.h"
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
 * Apply a SPILL record on a standby: clear the in-shared-memory
 * memtable for the index. The corresponding segment data and
 * metapage updates have already been replayed via GenericXLog
 * records emitted by segment.c / metapage.c during the primary's
 * tp_do_spill, so the standby's on-disk state is now in sync.
 *
 * If no registry entry yet exists for the index, skip — first
 * backend access will rebuild from the (now updated) docid pages
 * and metapage.
 */
static void
tp_redo_apply_spill(XLogReaderState *record)
{
	xl_tp_spill		   *hdr = (xl_tp_spill *)XLogRecGetData(record);
	TpSharedIndexState *shared;
	dsa_area		   *dsa;
	TpLocalIndexState	stack_local;

	{
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

	tp_acquire_index_lock(&stack_local, LW_EXCLUSIVE);
	tp_clear_memtable(&stack_local);
	tp_release_index_lock(&stack_local);
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
		appendStringInfo(buf, "index_oid %u", r->index_oid);
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

XLogRecPtr
tp_xlog_spill(Oid index_oid)
{
	xl_tp_spill hdr = {.index_oid = index_oid};

	XLogBeginInsert();
	XLogRegisterData((char *)&hdr, sizeof(hdr));
	return XLogInsert(TP_RMGR_ID, XLOG_TP_SPILL);
}
