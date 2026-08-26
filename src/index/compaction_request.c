/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_request.c - Background compaction request tracking
 */
#include <postgres.h>

#include <nodes/pg_list.h>
#include <utils/memutils.h>

#include "index/compaction_request.h"

int tp_compaction_mode = TP_COMPACTION_INLINE;

static List *tp_pending_compactions = NIL;

void
tp_compaction_request(Oid indexoid)
{
	MemoryContext oldcxt;

	/*
	 * Called from tp_do_spill() while the caller holds the per-index
	 * LWLock in LW_EXCLUSIVE mode.  Do no SPI, catalog access, relation
	 * opens, or ereport above DEBUG here; only append to this list.
	 */
	if (list_member_oid(tp_pending_compactions, indexoid))
		return;

	oldcxt				   = MemoryContextSwitchTo(TopMemoryContext);
	tp_pending_compactions = lappend_oid(tp_pending_compactions, indexoid);
	MemoryContextSwitchTo(oldcxt);
}
