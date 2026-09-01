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
#include <catalog/pg_class_d.h>
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
#include <utils/syscache.h>
#include <utils/varlena.h>

#include "index/compaction_request.h"

int	  tp_compaction_mode			 = TP_COMPACTION_INLINE;
char *tp_compaction_request_function = "";

static List *tp_pending_compactions = NIL;

typedef struct TpResolvedRequestFunction
{
	NameData namespace_name;
	NameData function_name;
} TpResolvedRequestFunction;

bool
tp_check_compaction_request_function(
		char		   **newval,
		void **extra	 pg_attribute_unused(),
		GucSource source pg_attribute_unused())
{
	char *rawname;
	List *names = NIL;
	bool  valid;

	if ((*newval)[0] == '\0')
		return true;

	rawname = pstrdup(*newval);
	valid	= SplitIdentifierString(rawname, '.', &names);
	valid	= valid && list_length(names) >= 1 && list_length(names) <= 2;
	if (valid)
	{
		foreach_ptr(char, name, names)
		{
			if (name[0] == '\0')
			{
				valid = false;
				break;
			}
		}
	}
	if (!valid)
		GUC_check_errdetail(
				"Must be empty, an unqualified identifier, or a "
				"schema-qualified identifier.");

	pfree(rawname);
	list_free(names);

	return valid;
}

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

static TpResolvedRequestFunction *
tp_lookup_request_function(void)
{
	MemoryContext oldcxt   = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	/*
	 * volatile: assigned in the PG_TRY section and dereferenced in the
	 * PG_CATCH section, so a longjmp must not restore a stale register
	 * copy (see the PG_TRY notes in utils/elog.h).
	 */
	TpResolvedRequestFunction *volatile resolved = NULL;
	Oid	 funcoid								 = InvalidOid;
	bool lookup_failed							 = false;

	if (tp_compaction_request_function == NULL ||
		tp_compaction_request_function[0] == '\0')
	{
		ereport(WARNING,
				(errmsg("bm25: pg_textsearch.compaction_request_function "
						"is unset or unresolvable; skipping background "
						"compaction")));
		return NULL;
	}

	/*
	 * Name and catalog resolution can ERROR on permissions or concurrent
	 * DDL.  Resolve and copy the complete qualified identity inside the
	 * same double-subtransaction shield as callback execution because this
	 * runs from PRE_COMMIT/PRE_PREPARE too.
	 */
	BeginInternalSubTransaction(NULL);
	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		MemoryContext lookupcxt = CurrentMemoryContext;
		List		 *names;
		Oid			  argtypes[1] = {REGCLASSOID};
		char		 *namespace_name;
		char		 *function_name;

		names = stringToQualifiedNameList(
				tp_compaction_request_function, NULL);
		funcoid = LookupFuncName(names, 1, argtypes, true);
		if (OidIsValid(funcoid))
		{
			namespace_name = get_namespace_name(get_func_namespace(funcoid));
			function_name  = get_func_name(funcoid);
			if (namespace_name == NULL || function_name == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("cache lookup failed for function %u",
								funcoid)));

			MemoryContextSwitchTo(oldcxt);
			resolved = palloc(sizeof(*resolved));
			namestrcpy(&resolved->namespace_name, namespace_name);
			namestrcpy(&resolved->function_name, function_name);
			MemoryContextSwitchTo(lookupcxt);
		}
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
	}
	PG_CATCH();
	{
		ErrorData *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();

		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		lookup_failed = true;
		if (resolved != NULL)
		{
			pfree(resolved);
			resolved = NULL;
		}

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
				(errmsg("bm25: background compaction callback lookup failed: "
						"%s; skipping background compaction",
						edata->message)));
		FreeErrorData(edata);
	}
	PG_END_TRY();

	ReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(oldcxt);
	CurrentResourceOwner = oldowner;

	if (resolved == NULL && !lookup_failed)
		ereport(WARNING,
				(errmsg("bm25: pg_textsearch.compaction_request_function "
						"is unset or unresolvable; skipping background "
						"compaction")));

	return resolved;
}

static void
tp_run_request(const TpResolvedRequestFunction *function, Oid indexoid)
{
	MemoryContext  oldcxt	= CurrentMemoryContext;
	ResourceOwner  oldowner = CurrentResourceOwner;
	StringInfoData sql;
	const char	  *idxname;

	/*
	 * Resolve the index name before entering the callback shield so a
	 * dropped request can still produce a stable diagnostic.
	 */
	idxname = get_rel_name(indexoid);
	if (idxname == NULL)
		idxname = "?";

	initStringInfo(&sql);
	appendStringInfo(
			&sql,
			"SELECT %s(%u::pg_catalog.oid::pg_catalog.regclass)",
			quote_qualified_identifier(
					NameStr(function->namespace_name),
					NameStr(function->function_name)),
			indexoid);

	/*
	 * PRE_COMMIT leaves the top-level block in TBLOCK_END.  An outer
	 * no-op subtransaction keeps the inner rollback on a normal
	 * TBLOCK_SUBINPROGRESS parent, which is safe on supported PostgreSQL
	 * versions.  The inner rollback also discards all callback-local
	 * effects.
	 */
	BeginInternalSubTransaction(NULL);
	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		PushActiveSnapshot(GetTransactionSnapshot());

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (SPI_execute(sql.data, false, 0) < 0)
			elog(ERROR, "SPI_execute failed");
		SPI_finish();

		PopActiveSnapshot();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
	}
	PG_CATCH();
	{
		ErrorData *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();

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

	ReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(oldcxt);
	CurrentResourceOwner = oldowner;

	pfree(sql.data);
}

void
tp_compaction_flush_requests(void)
{
	/*
	 * volatile: assigned in the PG_TRY section and read in the
	 * PG_FINALLY section, which also runs after a longjmp.
	 */
	TpResolvedRequestFunction *volatile function = NULL;
	ListCell *lc;
	List	 *pending;

	if (tp_pending_compactions == NIL)
		return;

	/* Take ownership so callback re-entry cannot loop. */
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
		foreach (lc, pending)
		{
			Oid indexoid = lfirst_oid(lc);

			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexoid)))
				continue;

			if (function == NULL)
			{
				function = tp_lookup_request_function();
				if (function == NULL)
					break;
			}

			tp_run_request(function, indexoid);
		}
	}
	PG_FINALLY();
	{
		if (function != NULL)
			pfree(function);
		list_free(pending);
	}
	PG_END_TRY();
}
