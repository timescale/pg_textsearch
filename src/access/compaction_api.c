/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction_api.c - BM25 index compaction inspection and control
 */
#include <postgres.h>

#include <access/htup_details.h>
#include <access/relation.h>
#include <access/xlog.h>
#include <catalog/objectaccess.h>
#include <catalog/pg_class.h>
#include <catalog/pg_type.h>
#include <miscadmin.h>
#include <utils/acl.h>
#include <utils/array.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>

#include "access/am.h"
#include "constants.h"
#include "index/compaction_request.h"
#include "index/metapage.h"
#include "index/state.h"
#include "segment/compaction.h"

typedef struct TpCompactionTarget
{
	Oid			  index_oid;
	Oid			  database_oid;
	Oid			  tablespace_oid;
	RelFileNumber relfilenumber;
	Oid			  owner_oid;
} TpCompactionTarget;

/*
 * Open a bm25 index by OID, validating that it is in fact a bm25
 * index and (optionally) that the caller owns it.
 */
static Relation
tp_open_bm25_index(Oid indexoid, LOCKMODE lockmode, bool need_owner)
{
	Relation index_rel;

	/*
	 * Reject nonowners before queuing for a heavyweight lock. Recheck after
	 * opening because ALTER OWNER can complete while this caller waits.
	 */
	if (need_owner &&
		!object_ownercheck(RelationRelationId, indexoid, GetUserId()))
	{
		char *relname = get_rel_name(indexoid);

		if (relname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("relation with OID %u does not exist", indexoid)));
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, relname);
	}

	index_rel = relation_open(indexoid, lockmode);

	if (index_rel->rd_indam == NULL ||
		index_rel->rd_indam->ambuild != tp_build)
	{
		char *relname = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a bm25 index", relname)));
	}

	if (index_rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
	{
		char *relname = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, lockmode);
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a partitioned bm25 index", relname),
				 errhint("Use a physical partition index instead.")));
	}

	if (need_owner &&
		!object_ownercheck(RelationRelationId, indexoid, GetUserId()))
	{
		char *relname = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, lockmode);
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, relname);
	}

	return index_rel;
}

/*
 * Return NULL when a captured worker target no longer names the same
 * physical bm25 index.
 */
static Relation
tp_open_current_bm25_target(
		const TpCompactionTarget *target,
		LOCKMODE				  lockmode,
		bool					  need_owner,
		bool					  need_background)
{
	Relation	  index_rel;
	HeapTuple	  tuple;
	Form_pg_class classform;

	if (target->database_oid != MyDatabaseId)
		return NULL;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(target->index_oid));
	if (!HeapTupleIsValid(tuple))
		return NULL;

	classform = (Form_pg_class)GETSTRUCT(tuple);
	if (classform->relowner != target->owner_oid)
	{
		ReleaseSysCache(tuple);
		return NULL;
	}

	/*
	 * Authorize against the captured owner so OID reuse cannot redirect the
	 * lockless precheck to a replacement relation.
	 */
	if (need_owner && !has_privs_of_role(GetUserId(), target->owner_oid))
	{
		char *relname = pstrdup(NameStr(classform->relname));

		ReleaseSysCache(tuple);
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, relname);
	}
	ReleaseSysCache(tuple);

	index_rel = try_relation_open(target->index_oid, lockmode);
	if (index_rel == NULL)
		return NULL;

	if (index_rel->rd_indam == NULL ||
		index_rel->rd_indam->ambuild != tp_build ||
		index_rel->rd_rel->relkind != RELKIND_INDEX ||
		index_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP ||
		index_rel->rd_locator.dbOid != target->database_oid ||
		index_rel->rd_locator.spcOid != target->tablespace_oid ||
		index_rel->rd_locator.relNumber != target->relfilenumber ||
		index_rel->rd_rel->relowner != target->owner_oid ||
		(need_background &&
		 tp_index_compaction_mode(index_rel) != TP_COMPACTION_BACKGROUND))
	{
		relation_close(index_rel, lockmode);
		return NULL;
	}

	if (need_owner &&
		!object_ownercheck(RelationRelationId, target->index_oid, GetUserId()))
	{
		char *relname = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, lockmode);
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_INDEX, relname);
	}

	return index_rel;
}

PG_FUNCTION_INFO_V1(tp_level_counts);

Datum
tp_level_counts(PG_FUNCTION_ARGS)
{
	Oid					 indexoid = PG_GETARG_OID(0);
	Relation			 index_rel;
	TpIndexMetaPageData *metap;
	Datum				 elems[TP_MAX_LEVELS];
	ArrayType			*result;
	int					 i;

	index_rel = tp_open_bm25_index(indexoid, AccessShareLock, false);

	metap = tp_get_metapage(index_rel);
	for (i = 0; i < TP_MAX_LEVELS; i++)
		elems[i] = Int32GetDatum((int32)metap->level_counts[i]);
	pfree(metap);

	relation_close(index_rel, AccessShareLock);

	result = construct_array(
			elems, TP_MAX_LEVELS, INT4OID, sizeof(int32), true, TYPALIGN_INT);
	PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(tp_compact_index);

Datum
tp_compact_index(PG_FUNCTION_ARGS)
{
	Oid				   indexoid = PG_GETARG_OID(0);
	Relation		   index_rel;
	TpLocalIndexState *index_state;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot compact a bm25 index during recovery")));

	index_rel = tp_open_bm25_index(indexoid, RowExclusiveLock, true);

	if (!RelationUsesLocalBuffers(index_rel))
		PreventCommandIfReadOnly("bm25 index compaction");

	index_state = tp_get_local_index_state(indexoid);
	if (index_state == NULL)
	{
		char *relname = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for \"%s\"", relname)));
	}

	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
	PG_TRY();
	{
		tp_maybe_compact_level(index_state, index_rel, 0);
	}
	PG_FINALLY();
	{
		tp_release_index_lock(index_state);
		relation_close(index_rel, RowExclusiveLock);
	}
	PG_END_TRY();

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(tp_compact_index_step);

Datum
tp_compact_index_step(PG_FUNCTION_ARGS)
{
	Oid				   indexoid = PG_GETARG_OID(0);
	Relation		   index_rel;
	TpLocalIndexState *index_state;
	bool			   pass_ran;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot compact a bm25 index during recovery")));

	index_rel = tp_open_bm25_index(indexoid, RowExclusiveLock, true);

	if (!RelationUsesLocalBuffers(index_rel))
		PreventCommandIfReadOnly("bm25 index compaction");

	index_state = tp_get_local_index_state(indexoid);
	if (index_state == NULL)
	{
		char *relname = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for \"%s\"", relname)));
	}

	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
	PG_TRY();
	{
		pass_ran = tp_compact_step(index_state, index_rel);
	}
	PG_FINALLY();
	{
		tp_release_index_lock(index_state);
		relation_close(index_rel, RowExclusiveLock);
	}
	PG_END_TRY();

	PG_RETURN_BOOL(pass_ran);
}

PG_FUNCTION_INFO_V1(tp_compact_index_step_if_current);

Datum
tp_compact_index_step_if_current(PG_FUNCTION_ARGS)
{
	TpCompactionTarget target = {
			.index_oid		= PG_GETARG_OID(0),
			.database_oid	= PG_GETARG_OID(1),
			.tablespace_oid = PG_GETARG_OID(2),
			.relfilenumber	= PG_GETARG_OID(3),
			.owner_oid		= PG_GETARG_OID(4),
	};
	Relation		   index_rel;
	TpLocalIndexState *index_state;
	bool			   pass_ran;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot compact a bm25 index during recovery")));

	index_rel = tp_open_current_bm25_target(
			&target, RowExclusiveLock, true, false);
	if (index_rel == NULL)
		PG_RETURN_BOOL(false);

	PreventCommandIfReadOnly("bm25 index compaction");

	index_state = tp_get_local_index_state(target.index_oid);
	if (index_state == NULL)
	{
		char *relname = pstrdup(RelationGetRelationName(index_rel));

		relation_close(index_rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get index state for \"%s\"", relname)));
	}

	tp_acquire_index_lock(index_state, LW_EXCLUSIVE);
	PG_TRY();
	{
		pass_ran = tp_compact_step(index_state, index_rel);
	}
	PG_FINALLY();
	{
		tp_release_index_lock(index_state);
		relation_close(index_rel, RowExclusiveLock);
	}
	PG_END_TRY();

	PG_RETURN_BOOL(pass_ran);
}

PG_FUNCTION_INFO_V1(tp_background_target_is_current);

Datum
tp_background_target_is_current(PG_FUNCTION_ARGS)
{
	TpCompactionTarget target = {
			.index_oid		= PG_GETARG_OID(0),
			.database_oid	= PG_GETARG_OID(1),
			.tablespace_oid = PG_GETARG_OID(2),
			.relfilenumber	= PG_GETARG_OID(3),
			.owner_oid		= PG_GETARG_OID(4),
	};
	Relation index_rel;

	index_rel =
			tp_open_current_bm25_target(&target, AccessShareLock, true, true);
	if (index_rel == NULL)
		PG_RETURN_BOOL(false);

	relation_close(index_rel, AccessShareLock);

	PG_RETURN_BOOL(true);
}
