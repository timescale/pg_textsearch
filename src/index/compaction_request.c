/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_request.c - Background compaction request tracking
 */
#include <postgres.h>

#include <access/parallel.h>
#include <access/xact.h>
#include <catalog/pg_class_d.h>
#include <miscadmin.h>
#include <nodes/pg_list.h>
#include <utils/lsyscache.h>
#include <utils/memutils.h>
#include <utils/resowner.h>
#include <utils/snapmgr.h>
#include <utils/syscache.h>

#include "access/am.h"
#include "index/compaction_job.h"
#include "index/compaction_request.h"

char *tp_background_compaction_schedule = NULL;

/*
 * Pending requests live in TopTransactionContext, which PostgreSQL frees
 * at commit, prepare, and abort alike, so no transaction callback has to
 * clear them.  A reset callback nulls this pointer when that happens.
 * The context outlives subtransactions, so a request from a rolled-back
 * savepoint survives, matching the spill it recorded.
 */
static List *tp_pending_compactions = NIL;
static bool	 tp_pending_registered	= false;

/* True while tp_compaction_flush_requests() is signaling managed jobs. */
static bool tp_dispatch_active = false;

/*
 * Can this process hand a compaction request off at commit?  Callers
 * that cannot compact inline instead, so the debt is never dropped.
 */
bool
tp_compaction_dispatch_possible(void)
{
	return !(
			IsParallelWorker() || IsInParallelMode() || RecoveryInProgress() ||
			AmAutoVacuumWorkerProcess() || tp_dispatch_active);
}

/* Spill-time compaction policy.  No reloption means inline. */
int
tp_index_compaction_mode(Relation index_rel)
{
	TpOptions *options = (TpOptions *)index_rel->rd_options;

	if (options == NULL)
		return TP_COMPACTION_INLINE;

	return options->compaction;
}

/* Per-index schedule override.  No reloption means use the global schedule. */
const char *
tp_index_compaction_schedule(Relation index_rel)
{
	TpOptions *options = (TpOptions *)index_rel->rd_options;

	if (options == NULL || options->compaction_schedule_offset == 0)
		return NULL;

	return (const char *)options + options->compaction_schedule_offset;
}

static void
tp_pending_compactions_reset(void *arg pg_attribute_unused())
{
	tp_pending_compactions = NIL;
	tp_pending_registered  = false;
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

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);

	if (!tp_pending_registered)
	{
		MemoryContextCallback *cb = palloc0(sizeof(*cb));

		cb->func = tp_pending_compactions_reset;
		MemoryContextRegisterResetCallback(TopTransactionContext, cb);
		tp_pending_registered = true;
	}

	tp_pending_compactions = lappend_oid(tp_pending_compactions, indexoid);
	MemoryContextSwitchTo(oldcxt);
}

static void
tp_run_request(Oid indexoid)
{
	MemoryContext oldcxt   = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	const char	 *idxname;

	/*
	 * Resolve the index name before entering the error shield so a
	 * dropped request can still produce a stable diagnostic.
	 */
	idxname = get_rel_name(indexoid);
	if (idxname == NULL)
		idxname = "?";

	/*
	 * PRE_COMMIT leaves the top-level block in TBLOCK_END.  An outer
	 * subtransaction keeps the recoverable inner transaction on a normal
	 * TBLOCK_SUBINPROGRESS parent.  A successful signal is released into
	 * the writer transaction; an ordinary failure rolls back only the
	 * inner transaction and becomes a warning.
	 */
	BeginInternalSubTransaction(NULL);
	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		tp_compaction_job_signal(indexoid);
		PopActiveSnapshot();
		ReleaseCurrentSubTransaction();
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
				(errmsg("bm25: managed background compaction request for "
						"index \"%s\" failed: %s",
						idxname,
						edata->message)));
		FreeErrorData(edata);
	}
	PG_END_TRY();

	ReleaseCurrentSubTransaction();
	MemoryContextSwitchTo(oldcxt);
	CurrentResourceOwner = oldowner;
}

void
tp_compaction_flush_requests(void)
{
	ListCell *lc;
	List	 *pending;

	if (tp_pending_compactions == NIL)
		return;

	/*
	 * Defensive: such a process never records a request, because the
	 * spill path compacts inline.  Discard rather than run callback SQL
	 * where it is unsafe; the debt is on disk and re-detected later.
	 */
	if (!tp_compaction_dispatch_possible())
	{
		list_free(tp_pending_compactions);
		tp_pending_compactions = NIL;
		return;
	}

	/* Take ownership so callback re-entry cannot loop. */
	pending				   = tp_pending_compactions;
	tp_pending_compactions = NIL;

	/*
	 * A spill caused during dispatch compacts inline: its request would
	 * land in a list this flush has stopped reading and be freed at
	 * commit, while the original spill debt remains durable.
	 */
	tp_dispatch_active = true;

	PG_TRY();
	{
		foreach (lc, pending)
		{
			Oid indexoid = lfirst_oid(lc);

			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexoid)))
				continue;
			tp_run_request(indexoid);
		}
	}
	PG_FINALLY();
	{
		tp_dispatch_active = false;
		list_free(pending);
	}
	PG_END_TRY();
}
