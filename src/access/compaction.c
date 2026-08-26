/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * compaction.c - BM25 index compaction inspection and control
 */
#include <postgres.h>

#include <access/relation.h>
#include <catalog/objectaccess.h>
#include <catalog/pg_class.h>
#include <catalog/pg_type.h>
#include <miscadmin.h>
#include <utils/acl.h>
#include <utils/array.h>

#include "access/am.h"
#include "constants.h"
#include "index/metapage.h"

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
