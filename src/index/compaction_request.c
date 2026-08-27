/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_request.c - Background compaction request tracking
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/xact.h>
#include <catalog/namespace.h>
#include <catalog/pg_type_d.h>
#include <executor/spi.h>
#include <lib/stringinfo.h>
#include <miscadmin.h>
#include <nodes/pg_list.h>
#include <parser/parse_func.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/regproc.h>
#include <utils/resowner.h>
#include <utils/snapmgr.h>

#include "index/compaction_request.h"

int	  tp_compaction_mode			 = TP_COMPACTION_INLINE;
char *tp_compaction_request_function = "";

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

void
tp_compaction_reset_requests(void)
{
	list_free(tp_pending_compactions);
	tp_pending_compactions = NIL;
}

static Oid
tp_lookup_request_function(void)
{
	List *names;
	Oid	  argtypes[1] = {REGCLASSOID};

	if (tp_compaction_request_function == NULL ||
		tp_compaction_request_function[0] == '\0')
		return InvalidOid;

	names = stringToQualifiedNameList(tp_compaction_request_function, NULL);
	return LookupFuncName(names, 1, argtypes, true);
}

static void
tp_run_request(Oid funcoid, Oid indexoid)
{
	MemoryContext  oldcxt	= CurrentMemoryContext;
	ResourceOwner  oldowner = CurrentResourceOwner;
	StringInfoData sql;
	char		  *nspname;
	char		  *funcname;
	const char	  *idxname;

	nspname	 = get_namespace_name(get_func_namespace(funcoid));
	funcname = get_func_name(funcoid);

	/*
	 * Resolve the index name up front, while we are still in a clean
	 * transaction state, so the failure warning below can name the index
	 * rather than printing an OID that varies from run to run.
	 */
	idxname = get_rel_name(indexoid);
	if (idxname == NULL)
		idxname = "?";

	initStringInfo(&sql);
	/*
	 * Pass the index as a numeric OID cast to regclass: this is
	 * search_path-independent, injection-proof, and resolves correctly in
	 * the worker's separate session.
	 */
	appendStringInfo(
			&sql,
			"SELECT %s(%u::oid::regclass)",
			quote_qualified_identifier(nspname, funcname),
			indexoid);

	/*
	 * Two nested subtransactions, not one -- this is deliberate and must
	 * not be "simplified".
	 *
	 * We run at XACT_EVENT_PRE_COMMIT, so the top-level transaction's
	 * blockState is TBLOCK_END.  BeginInternalSubTransaction() explicitly
	 * accepts TBLOCK_END, but the assertion at the tail of
	 * RollbackAndReleaseCurrentSubTransaction() (xact.c:4806 in PG 17.10
	 * and 18.4) omits TBLOCK_END from the states its parent may be in.
	 * Rolling a single subtransaction back from here therefore crashes an
	 * assert-enabled build.  This is an upstream oversight rather than a
	 * misuse on our part: stock PostgreSQL trips the very same assertion
	 * when a deferred constraint trigger written in PL/pgSQL catches an
	 * error in an EXCEPTION block.
	 *
	 * Interposing an outer subtransaction that performs no work of its
	 * own means the inner rollback pops to a parent in
	 * TBLOCK_SUBINPROGRESS, which the assertion does accept.  The outer
	 * one is then closed through ReleaseCurrentSubTransaction(), whose
	 * success path only requires the parent to be TRANS_INPROGRESS -- and
	 * CommitTransaction() does not set TRANS_COMMIT until well after the
	 * PRE_COMMIT callbacks have run.
	 */
	BeginInternalSubTransaction(NULL);
	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		/*
		 * PreCommit_Portals() has already torn the portal down by the
		 * time the PRE_COMMIT callbacks run, so no active snapshot is
		 * left for SPI to use.  Supply one for the duration of the call.
		 * On the error path AtSubAbort_Snapshot() pops it for us.
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (SPI_execute(sql.data, false, 0) < 0)
			elog(ERROR, "SPI_execute failed");
		SPI_finish();

		PopActiveSnapshot();

		/* Release the inner subtransaction. */
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
	}
	PG_CATCH();
	{
		ErrorData *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();

		/* Pops to the outer subtransaction, not to the commit block. */
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);

		if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED ||
			edata->sqlerrcode == ERRCODE_ADMIN_SHUTDOWN ||
			edata->sqlerrcode == ERRCODE_CRASH_SHUTDOWN)
		{
			ReleaseCurrentSubTransaction();
			MemoryContextSwitchTo(oldcxt);
			CurrentResourceOwner = oldowner;
			ReThrowError(edata);
		}

		ereport(WARNING,
				(errmsg("bm25: background compaction request for "
						"index \"%s\" failed: %s",
						idxname,
						edata->message)));
		FreeErrorData(edata);
	}
	PG_END_TRY();

	/* Release the outer shield subtransaction. */
	ReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(oldcxt);
	CurrentResourceOwner = oldowner;

	pfree(sql.data);
}

void
tp_compaction_flush_requests(void)
{
	Oid		  funcoid;
	ListCell *lc;
	List	 *pending;

	if (tp_pending_compactions == NIL)
		return;

	/* Take ownership so re-entry cannot loop. */
	pending				   = tp_pending_compactions;
	tp_pending_compactions = NIL;

	if (IsParallelWorker() || IsInParallelMode() || RecoveryInProgress() ||
		AmAutoVacuumWorkerProcess())
	{
		list_free(pending);
		return;
	}

	PG_TRY();
	{
		funcoid = tp_lookup_request_function();
		if (!OidIsValid(funcoid))
			ereport(WARNING,
					(errmsg("bm25: pg_textsearch.compaction_request_function "
							"is unset or unresolvable; skipping background "
							"compaction")));
		else
			foreach (lc, pending)
				tp_run_request(funcoid, lfirst_oid(lc));
	}
	PG_FINALLY();
	{
		list_free(pending);
	}
	PG_END_TRY();
}
