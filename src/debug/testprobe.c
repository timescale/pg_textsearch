/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * testprobe.c - Test-only entry points into storage primitives.
 *
 * No production caller can hand tp_record_free_index_page() an invalid
 * block: the tombstone paths bound their blocks first (issues #465,
 * #469), and a segment's page list comes from the segment itself.  The
 * primitive's own guard (issue #468) therefore needs a direct caller to
 * be testable, and the test module cannot be one -- the extension is
 * built -fvisibility=hidden, so its symbols are not linkable from
 * another module.
 *
 * Built only for a --enable-injection-points server, alongside the rest
 * of the test-only surface.  pg_textsearch_test declares these against
 * the pg_textsearch library.
 */
#include <postgres.h>

#ifdef USE_INJECTION_POINTS

#include <access/relation.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <utils/rel.h>
#include <utils/relcache.h>

#include "index/freepage.h"

PG_FUNCTION_INFO_V1(pg_textsearch_test_free_index_page);

/* Free `block` directly, bypassing the callers that bound it first. */
Datum
pg_textsearch_test_free_index_page(PG_FUNCTION_ARGS)
{
	Oid		 index_oid = PG_GETARG_OID(0);
	int64	 block	   = PG_GETARG_INT64(1);
	Relation index;

	if (!superuser())
		elog(ERROR, "must be superuser to free an index page directly");
	if (block < 0 || block > MaxBlockNumber)
		elog(ERROR, "invalid test block number");

	index = relation_open(index_oid, RowExclusiveLock);
	tp_record_free_index_page(index, (BlockNumber)block);
	relation_close(index, RowExclusiveLock);

	PG_RETURN_VOID();
}

#endif /* USE_INJECTION_POINTS */
