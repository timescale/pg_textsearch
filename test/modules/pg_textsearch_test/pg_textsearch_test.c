#include <postgres.h>

#include <access/genam.h>
#include <access/generic_xlog.h>
#include <executor/spi.h>
#include <fmgr.h>
#include <miscadmin.h>
#include <nodes/primnodes.h>
#include <nodes/readfuncs.h>
#include <storage/bufmgr.h>
#include <utils/builtins.h>
#include <utils/injection_point.h>
#include <utils/plancache.h>
#include <varatt.h>

#include "debug/injection.h"
#include "index/metapage.h"
#include "segment/tombstone.h"
#include "types/query.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_panic);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_worker_panic);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_segment_limit);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_reclaim_horizon_hold);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_legacy_segment);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_v5_segment_total_len);
PG_FUNCTION_INFO_V1(pg_textsearch_test_attach_vacuum_total_len);
PG_FUNCTION_INFO_V1(pg_textsearch_test_set_tombstone_link);

Datum
pg_textsearch_test_set_tombstone_link(PG_FUNCTION_ARGS)
{
	Oid				  index_oid = PG_GETARG_OID(0);
	bool			  at_head	= PG_GETARG_BOOL(1);
	int64			  block		= PG_GETARG_INT64(2);
	Relation		  index;
	Buffer			  buf;
	Page			  page;
	TpIndexMetaPage	  metap;
	BlockNumber		  head;
	GenericXLogState *state;

	if (!superuser())
		elog(ERROR, "must be superuser to corrupt a tombstone link");
	if (block < 0 || block > MaxBlockNumber)
		elog(ERROR, "invalid test block number");

	index = index_open(index_oid, AccessExclusiveLock);
	buf	  = ReadBuffer(index, TP_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	metap = (TpIndexMetaPage)PageGetContents(BufferGetPage(buf));
	if (metap->magic != TP_METAPAGE_MAGIC ||
		metap->version != TP_METAPAGE_VERSION)
		elog(ERROR, "expected a current BM25 metapage");
	head = metap->pending_free_head;
	if (head == InvalidBlockNumber)
		elog(ERROR, "expected a nonempty tombstone chain");

	if (!at_head)
	{
		TpTombstonePage t;

		UnlockReleaseBuffer(buf);
		buf = ReadBuffer(index, head);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		t = tp_tombstone_page(BufferGetPage(buf));
		if (t->magic != TP_TOMBSTONE_MAGIC ||
			t->next_page != InvalidBlockNumber)
			elog(ERROR, "expected a single tombstone page");
	}

	state = GenericXLogStart(index);
	page  = GenericXLogRegisterBuffer(state, buf, GENERIC_XLOG_FULL_IMAGE);
	if (at_head)
		((TpIndexMetaPage)PageGetContents(page))->pending_free_head =
				(BlockNumber)block;
	else
		tp_tombstone_page(page)->next_page = (BlockNumber)block;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buf);
	index_close(index, AccessExclusiveLock);
	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_panic(PG_FUNCTION_ARGS)
{
	TpInjectionCondition condition = {.pid = MyProcPid};
	const char			*point	   = TP_INJECTION_AFTER_SPILL_FINALIZE;

	if (!PG_ARGISNULL(0))
		point = text_to_cstring(PG_GETARG_TEXT_PP(0));

	InjectionPointAttach(
			point,
			"pg_textsearch",
			"tp_injection_panic",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_worker_panic(PG_FUNCTION_ARGS)
{
	TpInjectionCondition condition = {.pid = MyProcPid, .worker_only = true};
	const char			*point	   = TP_INJECTION_AFTER_SPILL_FINALIZE;

	if (!PG_ARGISNULL(0))
		point = text_to_cstring(PG_GETARG_TEXT_PP(0));

	InjectionPointAttach(
			point,
			"pg_textsearch",
			"tp_injection_panic",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_segment_limit(PG_FUNCTION_ARGS)
{
	TpInjectionSegmentCountLimit condition =
			{.pid = MyProcPid, .limit = PG_GETARG_INT32(0)};

	if (condition.limit < 1 || condition.limit > PG_UINT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("segment count limit must be between 1 and %u",
						PG_UINT16_MAX)));

	InjectionPointAttach(
			TP_INJECTION_SEGMENT_COUNT_LIMIT,
			"pg_textsearch",
			"tp_injection_segment_count_limit",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_reclaim_horizon_hold(PG_FUNCTION_ARGS)
{
	TpInjectionCondition condition = {.pid = MyProcPid};

	InjectionPointAttach(
			TP_INJECTION_RECLAIM_HORIZON,
			"pg_textsearch",
			"tp_injection_reclaim_horizon_hold",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

/* Return the hinted datum from the direct Limit -> IndexScan under test. */
static TpQuery *
test_plan_query(PlannedStmt *stmt)
{
	IndexScan *scan;
	OpExpr	  *op;
	Const	  *value;

	if (!IsA(stmt->planTree, Limit) ||
		!IsA(stmt->planTree->lefttree, IndexScan))
		elog(ERROR, "expected Limit over IndexScan");
	scan = (IndexScan *)stmt->planTree->lefttree;
	if (list_length(scan->indexorderby) != 1)
		elog(ERROR, "expected one index ordering expression");
	op	  = castNode(OpExpr, linitial(scan->indexorderby));
	value = castNode(Const, lsecond(op->args));
	if (value->constisnull || value->constbyval || value->constlen != -1)
		elog(ERROR, "expected a variable-length query constant");
	return (TpQuery *)DatumGetPointer(value->constvalue);
}

PG_FUNCTION_INFO_V1(pg_textsearch_test_query_hint_roundtrip);

Datum
pg_textsearch_test_query_hint_roundtrip(PG_FUNCTION_ARGS)
{
	char		   *sql		   = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int64			expected_k = PG_GETARG_INT64(1);
	SPIPlanPtr		prepared;
	CachedPlan	   *cached;
	PlannedStmt	   *stmt;
	TpQuery		   *query;
	TpQuery		   *copied;
	TpQuery		   *restored;
	TpQuerySeedHint hint;
	TpQuerySeedHint canonical;
	Size			base;
	bool			valid;

	SPI_connect();
	prepared = SPI_prepare(sql, 0, NULL);
	if (prepared == NULL)
		elog(ERROR, "could not prepare hint test query");
	cached = SPI_plan_get_cached_plan(prepared);
	if (cached == NULL || list_length(cached->stmt_list) != 1)
		elog(ERROR, "expected one cached statement");
	stmt  = linitial_node(PlannedStmt, cached->stmt_list);
	query = test_plan_query(stmt);
	base  = offsetof(TpQuery, data) + query->query_text_len + 1;
	if (!(query->flags & TPQUERY_FLAG_SEED_HINT) ||
		VARSIZE(query) != base + sizeof(hint))
		elog(ERROR, "expected a complete seed hint");
	memcpy(&hint, (char *)query + base, sizeof(hint));
	memset(&canonical, 0, sizeof(canonical));
	canonical.magic		  = TPQUERY_SEED_HINT_MAGIC;
	canonical.k			  = expected_k;
	canonical.selectivity = hint.selectivity;

	copied	 = test_plan_query(copyObject(stmt));
	restored = test_plan_query(stringToNode(nodeToString(stmt)));
	valid	 = hint.selectivity > 0 && hint.selectivity < 1 &&
			memcmp(&hint, &canonical, sizeof(hint)) == 0 &&
			VARSIZE(query) == VARSIZE(copied) &&
			VARSIZE(query) == VARSIZE(restored) &&
			memcmp(query, copied, VARSIZE(query)) == 0 &&
			memcmp(query, restored, VARSIZE(query)) == 0;
	/* SPI_prepare produces an unsaved plan: no ResourceOwner reference. */
	ReleaseCachedPlan(cached, NULL);
	SPI_freeplan(prepared);
	SPI_finish();
	PG_RETURN_BOOL(valid);
}

Datum
pg_textsearch_test_attach_legacy_segment(PG_FUNCTION_ARGS)
{
	int64				  total_tokens = PG_GETARG_INT64(0);
	TpInjectionTokenTotal condition;

	if (total_tokens < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("legacy segment token total must be nonnegative")));

	condition.pid		   = MyProcPid;
	condition.total_tokens = (uint64)total_tokens;
	InjectionPointAttach(
			TP_INJECTION_LEGACY_SEGMENT,
			"pg_textsearch",
			"tp_injection_legacy_segment",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_v5_segment_total_len(PG_FUNCTION_ARGS)
{
	int64				  total_tokens = PG_GETARG_INT64(0);
	TpInjectionTokenTotal condition;

	if (total_tokens < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("V5 segment token total must be nonnegative")));

	condition.pid		   = MyProcPid;
	condition.total_tokens = (uint64)total_tokens;
	InjectionPointAttach(
			TP_INJECTION_V5_SEGMENT_TOTAL_LEN,
			"pg_textsearch",
			"tp_injection_v5_segment_total_len",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}

Datum
pg_textsearch_test_attach_vacuum_total_len(PG_FUNCTION_ARGS)
{
	int64				  total_tokens = PG_GETARG_INT64(0);
	TpInjectionTokenTotal condition;

	if (total_tokens < 0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("VACUUM token total must be nonnegative")));

	condition.pid		   = MyProcPid;
	condition.total_tokens = (uint64)total_tokens;
	InjectionPointAttach(
			TP_INJECTION_VACUUM_TOTAL_LEN,
			"pg_textsearch",
			"tp_injection_vacuum_total_len",
			&condition,
			sizeof(condition));

	PG_RETURN_VOID();
}
