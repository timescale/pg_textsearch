/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * rls.c - BM25 and row-level security policy checks
 */
#include <postgres.h>

#include <access/genam.h>
#include <access/relation.h>
#include <access/table.h>
#include <commands/defrem.h>
#include <utils/lsyscache.h>
#include <utils/relcache.h>

#include "access/rls.h"
#include "constants.h"

static bool
relation_has_bm25_index(Relation rel)
{
	List	 *indexes;
	ListCell *lc;
	Oid		  bm25_am_oid;
	bool	  found = false;

	bm25_am_oid = get_am_oid("bm25", true);
	if (!OidIsValid(bm25_am_oid))
		return false;

	indexes = RelationGetIndexList(rel);

	foreach (lc, indexes)
	{
		Relation index = index_open(lfirst_oid(lc), AccessShareLock);

		if (index->rd_rel->relam == bm25_am_oid)
			found = true;

		index_close(index, AccessShareLock);
		if (found)
			break;
	}

	list_free(indexes);
	return found;
}

void
tp_check_bm25_build_allowed(Relation heap)
{
	if (tp_allow_rls || !heap->rd_rel->relrowsecurity)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("BM25 indexes are not allowed on row-level security "
					"relation \"%s\" when "
					"\"pg_textsearch.allow_rls\" is off",
					RelationGetRelationName(heap)),
			 errhint("Set pg_textsearch.allow_rls to on to accept the "
					 "documented term-frequency leakage risk.")));
}

void
tp_check_rls_enable_allowed(Oid relid)
{
	Relation rel;

	if (tp_allow_rls)
		return;

	rel = table_open(relid, NoLock);
	if (!rel->rd_rel->relrowsecurity)
	{
		table_close(rel, NoLock);
		return;
	}

	if (relation_has_bm25_index(rel))
	{
		char *relname = pstrdup(RelationGetRelationName(rel));

		table_close(rel, NoLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot enable row-level security on relation "
						"\"%s\" because it has a BM25 index while "
						"\"pg_textsearch.allow_rls\" is off",
						relname),
				 errhint("Set pg_textsearch.allow_rls to on to accept the "
						 "documented term-frequency leakage risk.")));
	}
	table_close(rel, NoLock);
}
