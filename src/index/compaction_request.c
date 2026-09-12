/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_request.c - Background compaction request tracking
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/parallel.h>
#include <access/table.h>
#include <access/xact.h>
#include <catalog/pg_class.h>
#include <catalog/pg_class_d.h>
#include <commands/defrem.h>
#include <commands/tablecmds.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
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

const char *
tp_index_compaction_lineage(Relation index_rel)
{
	TpOptions *options = (TpOptions *)index_rel->rd_options;

	if (options == NULL || options->compaction_lineage_offset == 0)
		return NULL;

	return (const char *)options + options->compaction_lineage_offset;
}

char *
tp_new_compaction_lineage(void)
{
	static const char digits[] = "0123456789abcdef";
	unsigned char	  bytes[TP_COMPACTION_LINEAGE_BYTES];
	char			 *lineage;

	if (!pg_strong_random(bytes, sizeof(bytes)))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate background compaction lineage")));

	lineage = palloc(sizeof(bytes) * 2 + 1);
	for (Size i = 0; i < sizeof(bytes); i++)
	{
		lineage[i * 2]	   = digits[bytes[i] >> 4];
		lineage[i * 2 + 1] = digits[bytes[i] & 0x0f];
	}
	lineage[sizeof(bytes) * 2] = '\0';
	return lineage;
}

static bool
tp_live_compaction_lineage_exists(const char *lineage)
{
	Relation	class_rel;
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;

	class_rel = table_open(RelationRelationId, AccessShareLock);
	scan = systable_beginscan(class_rel, InvalidOid, false, NULL, 0, NULL);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_class class_form = (Form_pg_class)GETSTRUCT(tuple);
		Datum		  reloptions;
		bool		  isnull;
		List		 *options;
		ListCell	 *lc;

		if (class_form->relkind != RELKIND_INDEX &&
			class_form->relkind != RELKIND_PARTITIONED_INDEX)
			continue;

		reloptions = heap_getattr(
				tuple,
				Anum_pg_class_reloptions,
				RelationGetDescr(class_rel),
				&isnull);
		if (isnull)
			continue;

		options = untransformRelOptions(reloptions);
		foreach (lc, options)
		{
			DefElem *option = lfirst_node(DefElem, lc);

			if (strcmp(option->defname, "compaction_lineage") == 0 &&
				strcmp(defGetString(option), lineage) == 0)
			{
				found = true;
				break;
			}
		}
		list_free_deep(options);
		if (found)
			break;
	}
	systable_endscan(scan);
	table_close(class_rel, AccessShareLock);
	return found;
}

bool
tp_compaction_lineage_in_use(const char *lineage)
{
	return tp_live_compaction_lineage_exists(lineage) ||
		   tp_compaction_job_lineage_exists(lineage);
}

static char *
tp_new_available_compaction_lineage(void)
{
	char *lineage;

	do
	{
		lineage = tp_new_compaction_lineage();
		if (!tp_compaction_lineage_in_use(lineage))
			return lineage;
		pfree(lineage);
	} while (true);
}

char *
tp_ensure_index_compaction_lineage(Oid indexoid, bool *created)
{
	AlterTableCmd *cmd;
	Relation	   index_rel;
	const char	  *existing;
	char		  *lineage;
	Oid			   owner_oid;
	Oid			   save_userid;
	int			   save_sec_context;

	if (created != NULL)
		*created = false;

	index_rel = try_index_open(indexoid, AccessShareLock);
	if (index_rel == NULL)
		return NULL;
	if (index_rel->rd_indam == NULL ||
		index_rel->rd_indam->ambuild != tp_build ||
		index_rel->rd_rel->relkind != RELKIND_INDEX ||
		tp_index_compaction_mode(index_rel) != TP_COMPACTION_BACKGROUND)
	{
		index_close(index_rel, AccessShareLock);
		return NULL;
	}

	existing = tp_index_compaction_lineage(index_rel);
	if (existing != NULL)
	{
		lineage = pstrdup(existing);
		index_close(index_rel, AccessShareLock);
		return lineage;
	}

	/* Upgrade a pre-lineage background index under its physical owner. */
	owner_oid = index_rel->rd_rel->relowner;
	index_close(index_rel, NoLock);
	lineage = tp_new_available_compaction_lineage();

	cmd			  = makeNode(AlterTableCmd);
	cmd->subtype  = AT_SetRelOptions;
	cmd->def	  = (Node *)list_make1(makeDefElem(
			 "compaction_lineage", (Node *)makeString(lineage), -1));
	cmd->behavior = DROP_RESTRICT;

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(
			owner_oid, save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		AlterTableInternal(indexoid, list_make1(cmd), false);
		CommandCounterIncrement();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();

	if (created != NULL)
		*created = true;
	return lineage;
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
