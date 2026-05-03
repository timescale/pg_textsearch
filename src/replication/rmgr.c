/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * rmgr.c - Custom WAL resource manager for pg_textsearch
 *
 * Skeleton in this revision: registration + describe/identify
 * callbacks + emission stubs. Redo callbacks are stubs that are
 * filled in by subsequent tasks (INSERT_TERMS in Task 2.3, SPILL
 * in Task 2.4).
 */
#include <postgres.h>

#include <access/xlog.h>
#include <access/xlog_internal.h>
#include <access/xloginsert.h>
#include <access/xlogreader.h>
#include <lib/stringinfo.h>

#include "replication/replication.h"

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

void
tp_rmgr_redo(XLogReaderState *record)
{
	uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
	case XLOG_TP_INSERT_TERMS:
		/* Filled in by Task 2.3. */
		break;
	case XLOG_TP_SPILL:
		/* Filled in by Task 2.4. */
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
 * Emission helpers — stubs, replaced in Tasks 2.3 / 2.4. The stubs
 * intentionally do nothing so that this scaffolding commit is a
 * pure no-behavior-change addition.
 */
XLogRecPtr
tp_xlog_insert_terms(Oid index_oid, ItemPointer ctid, const TpVector *vec)
{
	(void)index_oid;
	(void)ctid;
	(void)vec;
	return InvalidXLogRecPtr;
}

XLogRecPtr
tp_xlog_spill(Oid index_oid)
{
	(void)index_oid;
	return InvalidXLogRecPtr;
}
