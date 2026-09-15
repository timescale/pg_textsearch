/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_request.c - Background compaction request tracking
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/htup_details.h>
#include <access/parallel.h>
#include <access/relation.h>
#include <access/table.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <catalog/partition.h>
#include <catalog/pg_am_d.h>
#include <catalog/pg_class.h>
#include <catalog/pg_class_d.h>
#include <commands/defrem.h>
#include <commands/tablecmds.h>
#include <common/hashfn.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/pg_list.h>
#include <storage/lmgr.h>
#include <storage/lock.h>
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

#define TP_COMPACTION_INDEX_LOCK_SUBID	 1
#define TP_COMPACTION_LINEAGE_LOCK_SUBID 2

static void
tp_set_compaction_locktag(LOCKTAG *tag, Oid object_id, uint16 discriminator)
{
	/*
	 * pg_am has no subobjects, so its object-subid space is private to the
	 * access method.  LOCKTAG_OBJECT also keeps these locks disjoint from
	 * SQL-visible advisory locks.
	 */
	SET_LOCKTAG_OBJECT(
			*tag,
			MyDatabaseId,
			AccessMethodRelationId,
			object_id,
			discriminator);
}

static bool
tp_compaction_lock_held(Oid object_id, uint16 discriminator, LOCKMODE lockmode)
{
	LOCKTAG tag;

	tp_set_compaction_locktag(&tag, object_id, discriminator);
	return LockHeldByMe(&tag, lockmode, true);
}

static bool
tp_compaction_dependency_lock_held(void)
{
	Oid bm25_am_oid = get_index_am_oid("bm25", true);

	if (!OidIsValid(bm25_am_oid))
		return false;
	return tp_compaction_lock_held(bm25_am_oid, 0, ShareRowExclusiveLock);
}

static bool
tp_compaction_admission_allowed(Oid indexoid)
{
	return tp_compaction_lock_held(
				   indexoid, TP_COMPACTION_INDEX_LOCK_SUBID, ExclusiveLock) ||
		   !tp_compaction_dependency_lock_held();
}

static void
tp_check_compaction_admission_order(Oid indexoid)
{
	if (tp_compaction_admission_allowed(indexoid))
		return;

	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("cannot acquire a new background compaction admission "
					"lock after pg_durable work has started in this "
					"transaction"),
			 errhint("Commit or roll back the current transaction before "
					 "managing another BM25 index.")));
}

static bool
tp_take_compaction_lock(
		Oid object_id, uint16 discriminator, LOCKMODE lockmode, bool nowait)
{
	LOCKTAG tag;

	tp_set_compaction_locktag(&tag, object_id, discriminator);
	if (LockHeldByMe(&tag, lockmode, true))
		return true;
	if (discriminator == TP_COMPACTION_INDEX_LOCK_SUBID)
		tp_check_compaction_admission_order(object_id);
	if (nowait)
		return ConditionalLockDatabaseObject(
				AccessMethodRelationId, object_id, discriminator, lockmode);
	LockDatabaseObject(
			AccessMethodRelationId, object_id, discriminator, lockmode);
	return true;
}

void
tp_lock_compaction_index(Oid indexoid)
{
	(void)tp_take_compaction_lock(
			indexoid, TP_COMPACTION_INDEX_LOCK_SUBID, ExclusiveLock, false);
}

void
tp_lock_compaction_dependency(void)
{
	Oid bm25_am_oid = get_index_am_oid("bm25", false);

	if (tp_compaction_dependency_lock_held())
		return;
	LockDatabaseObject(
			AccessMethodRelationId, bm25_am_oid, 0, ShareRowExclusiveLock);
}

void
tp_require_compaction_dependency_lock(void)
{
	if (!tp_compaction_dependency_lock_held())
		elog(ERROR, "managed compaction dependency lock is not held");
}

void
tp_require_compaction_index_lock(Oid indexoid)
{
	if (!tp_compaction_lock_held(
				indexoid, TP_COMPACTION_INDEX_LOCK_SUBID, ExclusiveLock))
		elog(ERROR, "managed compaction admission lock is not held");
}

void
tp_lock_compaction_lineage(const char *lineage)
{
	uint32 hash;

	hash = hash_bytes(
			(const unsigned char *)lineage, TP_COMPACTION_LINEAGE_LENGTH);
	(void)tp_take_compaction_lock(
			hash, TP_COMPACTION_LINEAGE_LOCK_SUBID, ExclusiveLock, false);
}

List *
tp_prelock_compaction_indexes_nowait(List *indexoids, bool nowait)
{
	List	 *sorted = list_copy(indexoids);
	List	 *locked = NIL;
	ListCell *lc;
	Oid		  previous = InvalidOid;

	list_sort(sorted, list_oid_cmp);
	foreach (lc, sorted)
	{
		Oid indexoid = lfirst_oid(lc);

		if (!OidIsValid(indexoid) || indexoid == previous)
			continue;
		tp_check_compaction_admission_order(indexoid);
		if (nowait)
		{
			if (!ConditionalLockRelationOid(
						indexoid, ShareUpdateExclusiveLock))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation with OID "
								"%u",
								indexoid)));
		}
		else
			LockRelationOid(indexoid, ShareUpdateExclusiveLock);
		locked	 = lappend_oid(locked, indexoid);
		previous = indexoid;
	}
	list_free(sorted);

	foreach (lc, locked)
	{
		Oid indexoid = lfirst_oid(lc);

		if (nowait)
		{
			if (!tp_take_compaction_lock(
						indexoid,
						TP_COMPACTION_INDEX_LOCK_SUBID,
						ExclusiveLock,
						true))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation with OID "
								"%u",
								indexoid)));
		}
		else
			tp_lock_compaction_index(indexoid);
	}
	return locked;
}

List *
tp_prelock_compaction_indexes(List *indexoids)
{
	return tp_prelock_compaction_indexes_nowait(indexoids, false);
}

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
tp_heaps_share_partition_family(Oid first_heap_oid, Oid second_heap_oid)
{
	List *ancestors;
	Oid	  first_root  = first_heap_oid;
	Oid	  second_root = second_heap_oid;

	if (get_rel_relispartition(first_heap_oid))
	{
		ancestors = get_partition_ancestors(first_heap_oid);
		if (ancestors != NIL)
			first_root = llast_oid(ancestors);
		list_free(ancestors);
	}
	if (get_rel_relispartition(second_heap_oid))
	{
		ancestors = get_partition_ancestors(second_heap_oid);
		if (ancestors != NIL)
			second_root = llast_oid(ancestors);
		list_free(ancestors);
	}

	return first_root == second_root &&
		   get_rel_relkind(first_root) == RELKIND_PARTITIONED_TABLE;
}

static bool
tp_live_compaction_lineage_conflicts(const char *lineage, Oid heap_oid)
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
				Oid existing_heap_oid =
						IndexGetRelation(class_form->oid, true);

				if (!OidIsValid(existing_heap_oid) ||
					existing_heap_oid == heap_oid ||
					!tp_heaps_share_partition_family(
							existing_heap_oid, heap_oid))
				{
					found = true;
					break;
				}
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
tp_compaction_lineage_in_use(const char *lineage, Oid heap_oid, Oid owner_oid)
{
	return tp_live_compaction_lineage_conflicts(lineage, heap_oid) ||
		   tp_compaction_job_lineage_exists(lineage, heap_oid, owner_oid);
}

char *
tp_new_available_compaction_lineage(Oid heap_oid, Oid owner_oid)
{
	char *lineage;

	do
	{
		lineage = tp_new_compaction_lineage();
		if (tp_compaction_lineage_in_use(lineage, heap_oid, owner_oid))
		{
			pfree(lineage);
			continue;
		}

		tp_lock_compaction_lineage(lineage);
		if (!tp_compaction_lineage_in_use(lineage, heap_oid, owner_oid))
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
	Oid			   heap_oid;
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
		index_close(index_rel, NoLock);
		return lineage;
	}
	index_close(index_rel, AccessShareLock);

	tp_check_compaction_admission_order(indexoid);
	index_rel = try_index_open(indexoid, ShareUpdateExclusiveLock);
	if (index_rel == NULL)
		return NULL;
	tp_lock_compaction_index(indexoid);
	if (index_rel->rd_indam == NULL ||
		index_rel->rd_indam->ambuild != tp_build ||
		index_rel->rd_rel->relkind != RELKIND_INDEX ||
		tp_index_compaction_mode(index_rel) != TP_COMPACTION_BACKGROUND)
	{
		index_close(index_rel, NoLock);
		return NULL;
	}

	existing = tp_index_compaction_lineage(index_rel);
	if (existing != NULL)
	{
		lineage = pstrdup(existing);
		index_close(index_rel, NoLock);
		return lineage;
	}

	/* Upgrade a pre-lineage background index under its physical owner. */
	heap_oid  = index_rel->rd_index->indrelid;
	owner_oid = index_rel->rd_rel->relowner;
	index_close(index_rel, NoLock);
	lineage = tp_new_available_compaction_lineage(heap_oid, owner_oid);

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

void
tp_reconcile_index_compaction_options(
		Oid indexoid, const char *schedule, const char *lineage)
{
	AlterTableCmd *cmd;
	Relation	   index_rel;
	const char	  *existing_lineage;
	const char	  *existing_schedule;
	List		  *commands	   = NIL;
	List		  *set_options = NIL;
	Oid			   owner_oid;
	Oid			   save_userid;
	int			   save_sec_context;
	bool		   schedule_matches;
	bool		   reset_schedule;

	index_rel = try_index_open(indexoid, ShareUpdateExclusiveLock);
	if (index_rel == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("partition index with OID %u disappeared", indexoid)));

	if (index_rel->rd_indam == NULL ||
		index_rel->rd_indam->ambuild != tp_build ||
		index_rel->rd_rel->relkind != RELKIND_INDEX ||
		index_rel->rd_index == NULL || !index_rel->rd_index->indisvalid ||
		!index_rel->rd_index->indisready || !index_rel->rd_index->indislive)
	{
		char *index_name = pstrdup(RelationGetRelationName(index_rel));

		index_close(index_rel, ShareUpdateExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot reconcile partition index \"%s\" for "
						"background compaction",
						index_name),
				 errdetail(
						 "The attached index is not a valid, ready "
						 "physical BM25 index."),
				 errhint("Drop or rebuild the incompatible child index before "
						 "creating the partitioned background index.")));
	}

	existing_lineage  = tp_index_compaction_lineage(index_rel);
	existing_schedule = tp_index_compaction_schedule(index_rel);
	schedule_matches  = schedule == NULL ? existing_schedule == NULL
										 : existing_schedule != NULL &&
												   strcmp(existing_schedule,
														  schedule) == 0;
	reset_schedule	  = schedule == NULL && existing_schedule != NULL;
	if (tp_index_compaction_mode(index_rel) == TP_COMPACTION_BACKGROUND &&
		existing_lineage != NULL && strcmp(existing_lineage, lineage) == 0 &&
		schedule_matches)
	{
		index_close(index_rel, NoLock);
		return;
	}

	owner_oid = index_rel->rd_rel->relowner;
	index_close(index_rel, NoLock);

	set_options = lappend(
			set_options,
			makeDefElem("compaction", (Node *)makeString("background"), -1));
	set_options =
			lappend(set_options,
					makeDefElem(
							"compaction_lineage",
							(Node *)makeString(pstrdup(lineage)),
							-1));
	if (schedule != NULL)
		set_options =
				lappend(set_options,
						makeDefElem(
								"compaction_schedule",
								(Node *)makeString(pstrdup(schedule)),
								-1));

	cmd			  = makeNode(AlterTableCmd);
	cmd->subtype  = AT_SetRelOptions;
	cmd->def	  = (Node *)set_options;
	cmd->behavior = DROP_RESTRICT;
	commands	  = lappend(commands, cmd);

	if (reset_schedule)
	{
		cmd			 = makeNode(AlterTableCmd);
		cmd->subtype = AT_ResetRelOptions;
		cmd->def	 = (Node *)list_make1(
				makeDefElem("compaction_schedule", NULL, -1));
		cmd->behavior = DROP_RESTRICT;
		commands	  = lappend(commands, cmd);
	}

	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(
			owner_oid, save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	PG_TRY();
	{
		AlterTableInternal(indexoid, commands, false);
		CommandCounterIncrement();
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(save_userid, save_sec_context);
	}
	PG_END_TRY();
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
	 * TBLOCK_SUBINPROGRESS parent.  A successful call releases the inner
	 * subtransaction into the writer transaction.  pg_durable delivers the
	 * event through an independent connection, so its worker may observe the
	 * already-published index state before the writer commits.  An ordinary
	 * failure rolls back only the inner transaction and becomes a warning.
	 */
	BeginInternalSubTransaction(NULL);
	BeginInternalSubTransaction(NULL);
	PG_TRY();
	{
		PushActiveSnapshot(GetLatestSnapshot());
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

static bool
tp_request_target_is_background(Oid indexoid)
{
	Relation index_rel;
	bool	 background;

	index_rel = try_relation_open(indexoid, NoLock);
	if (index_rel == NULL)
		return false;
	background = index_rel->rd_rel->relkind == RELKIND_INDEX &&
				 index_rel->rd_indam != NULL &&
				 index_rel->rd_indam->ambuild == tp_build &&
				 index_rel->rd_index != NULL &&
				 index_rel->rd_index->indisvalid &&
				 index_rel->rd_index->indisready &&
				 index_rel->rd_index->indislive &&
				 tp_index_compaction_mode(index_rel) ==
						 TP_COMPACTION_BACKGROUND;
	relation_close(index_rel, NoLock);
	return background;
}

static List *
tp_prelock_requests(List *pending)
{
	List	 *sorted  = list_copy(pending);
	List	 *targets = NIL;
	ListCell *lc;
	Oid		  previous = InvalidOid;

	list_sort(sorted, list_oid_cmp);
	foreach (lc, sorted)
	{
		Oid		 indexoid = lfirst_oid(lc);
		Relation index_rel;
		bool	 needs_lineage_lock;

		if (!OidIsValid(indexoid) || indexoid == previous)
			continue;
		previous = indexoid;

		/*
		 * Dispatch is best effort and the on-disk debt is durable.  If an
		 * earlier managed statement pinned the transaction dependency, do
		 * not take a new admission during PRE_COMMIT.
		 */
		if (!tp_compaction_admission_allowed(indexoid))
			continue;
		if (!ConditionalLockRelationOid(indexoid, AccessShareLock))
			continue;
		index_rel = try_relation_open(indexoid, NoLock);
		if (index_rel == NULL || index_rel->rd_rel->relkind != RELKIND_INDEX ||
			index_rel->rd_indam == NULL ||
			index_rel->rd_indam->ambuild != tp_build ||
			index_rel->rd_index == NULL || !index_rel->rd_index->indisvalid ||
			!index_rel->rd_index->indisready ||
			!index_rel->rd_index->indislive ||
			tp_index_compaction_mode(index_rel) != TP_COMPACTION_BACKGROUND)
		{
			if (index_rel != NULL)
				relation_close(index_rel, NoLock);
			UnlockRelationOid(indexoid, AccessShareLock);
			continue;
		}

		needs_lineage_lock = tp_index_compaction_lineage(index_rel) == NULL;
		relation_close(index_rel, NoLock);
		if (needs_lineage_lock)
		{
			UnlockRelationOid(indexoid, AccessShareLock);
			if (!ConditionalLockRelationOid(
						indexoid, ShareUpdateExclusiveLock))
				continue;
		}

		targets = lappend_oid(targets, indexoid);
	}
	list_free(sorted);

	foreach (lc, targets)
	{
		Oid indexoid = lfirst_oid(lc);

		tp_lock_compaction_index(indexoid);
	}
	return targets;
}

void
tp_compaction_flush_requests(void)
{
	ListCell *lc;
	List	 *pending;
	List	 *targets = NIL;

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
		targets = tp_prelock_requests(pending);
		foreach (lc, targets)
		{
			Oid indexoid = lfirst_oid(lc);

			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexoid)) ||
				!tp_request_target_is_background(indexoid))
				continue;
			tp_run_request(indexoid);
		}
	}
	PG_FINALLY();
	{
		tp_dispatch_active = false;
		list_free(targets);
		list_free(pending);
	}
	PG_END_TRY();
}
