/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction.c - BM25 index compaction inspection and control
 */
#include <postgres.h>

#include <access/relation.h>
#include <access/xlog.h>
#include <catalog/objectaccess.h>
#include <catalog/pg_class.h>
#include <catalog/pg_type.h>
#include <miscadmin.h>
#include <utils/acl.h>
#include <utils/array.h>

#include "access/am.h"
#include "constants.h"
#include "index/metapage.h"
#include "index/state.h"
#include "segment/merge.h"

/*
 * Open a bm25 index by OID, validating that it is in fact a bm25
 * index and (optionally) that the caller owns it.
 */
static Relation
tp_open_bm25_index(Oid indexoid, LOCKMODE lockmode, bool need_owner)
{
	Relation index_rel;

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

	if (need_owner &&
		!object_ownercheck(RelationRelationId, indexoid, GetUserId()))
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
		tp_maybe_compact_level(index_rel, 0);
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
	bool			   more_work;

	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot compact a bm25 index during recovery")));

	index_rel = tp_open_bm25_index(indexoid, RowExclusiveLock, true);

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
		more_work = tp_compact_step(index_rel);
	}
	PG_FINALLY();
	{
		tp_release_index_lock(index_state);
		relation_close(index_rel, RowExclusiveLock);
	}
	PG_END_TRY();

	PG_RETURN_BOOL(more_work);
}
