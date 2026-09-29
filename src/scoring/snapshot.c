/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * One published index generation per executor, shared by ranked scans and
 * score expressions. No buffer or LWLock survives a scoring call.
 */
#include <postgres.h>

#include <executor/executor.h>
#include <miscadmin.h>
#include <storage/lmgr.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "index/state.h"
#include "memtable/cache_source.h"
#include "scoring/snapshot.h"

typedef struct TpScoringIndex
{
	Oid					   index_oid;
	RelFileLocator		   locator;
	TpScoringSnapshot	   snapshot;
	struct TpScoringIndex *next;
} TpScoringIndex;

typedef struct TpScoringStatement
{
	MemoryContext			   context;
	MemoryContextCallback	   cleanup;
	TpScoringIndex			  *indexes;
	struct TpScoringStatement *next;
} TpScoringStatement;

static ExecutorRun_hook_type	previous_run;
static ExecutorFinish_hook_type previous_finish;
static MemoryContext			current_query_context;
static TpScoringStatement	   *statements;
static uint64					next_serial;

static void
tp_scoring_statement_cleanup(void *arg)
{
	TpScoringStatement	*statement = arg;
	TpScoringStatement **link	   = &statements;

	while (*link != statement)
	{
		Assert(*link != NULL);
		link = &(*link)->next;
	}
	*link = statement->next;
}

static void
tp_scoring_executor_run(
		QueryDesc	 *query,
		ScanDirection direction,
		uint64		  count
#if PG_VERSION_NUM < 180000
		,
		bool execute_once
#endif
)
{
	MemoryContext previous_context = current_query_context;

	current_query_context = query->estate->es_query_cxt;
	PG_TRY();
	{
		if (previous_run)
			previous_run(
					query,
					direction,
					count
#if PG_VERSION_NUM < 180000
					,
					execute_once
#endif
			);
		else
			standard_ExecutorRun(
					query,
					direction,
					count
#if PG_VERSION_NUM < 180000
					,
					execute_once
#endif
			);
	}
	PG_FINALLY();
	{
		current_query_context = previous_context;
	}
	PG_END_TRY();
}

static void
tp_scoring_executor_finish(QueryDesc *query)
{
	MemoryContext previous_context = current_query_context;

	current_query_context = query->estate->es_query_cxt;
	PG_TRY();
	{
		if (previous_finish)
			previous_finish(query);
		else
			standard_ExecutorFinish(query);
	}
	PG_FINALLY();
	{
		current_query_context = previous_context;
	}
	PG_END_TRY();
}

void
tp_scoring_snapshot_init(void)
{
	previous_run		= ExecutorRun_hook;
	ExecutorRun_hook	= tp_scoring_executor_run;
	previous_finish		= ExecutorFinish_hook;
	ExecutorFinish_hook = tp_scoring_executor_finish;
}

TpScoringSnapshot *
tp_scoring_snapshot_get(
		Relation		   index,
		const char *const *query_terms,
		int				   query_term_count,
		TpDataSource	 **initial_source)
{
	MemoryContext		context = current_query_context != NULL
										? current_query_context
										: CurrentMemoryContext;
	MemoryContext		old;
	TpScoringStatement *statement;
	TpScoringIndex	   *entry;
	TpDataSource	   *source;
	TpLocalIndexState  *state;
	const char		   *no_terms = NULL;
	int64				total_docs;
	int64				total_len;

	if (initial_source != NULL)
		*initial_source = NULL;
	for (statement = statements; statement; statement = statement->next)
		if (statement->context == context)
			break;
	if (statement == NULL)
	{
		statement = MemoryContextAllocZero(context, sizeof(*statement));
		statement->context		= context;
		statement->cleanup.func = tp_scoring_statement_cleanup;
		statement->cleanup.arg	= statement;
		MemoryContextRegisterResetCallback(context, &statement->cleanup);
		statement->next = statements;
		statements		= statement;
	}
	for (entry = statement->indexes; entry; entry = entry->next)
		if (entry->index_oid == RelationGetRelid(index) &&
			RelFileLocatorEquals(entry->locator, index->rd_locator))
			return &entry->snapshot;

	/* Exclude concurrent physical rewrites between standalone calls. */
	LockRelationOid(RelationGetRelid(index), AccessShareLock);
	state = tp_get_local_index_state(RelationGetRelid(index));
	if (state == NULL)
		elog(ERROR, "could not get BM25 index state");

	old						 = MemoryContextSwitchTo(context);
	entry					 = palloc0(sizeof(*entry));
	entry->index_oid		 = RelationGetRelid(index);
	entry->locator			 = index->rd_locator;
	entry->snapshot.serial	 = ++next_serial;
	entry->snapshot.recovery = RecoveryInProgress();
	entry->snapshot.graph	 = tp_segment_graph_snapshot_create(index);
	MemoryContextSwitchTo(old);

	source = tp_memtable_source_create_for_snapshot(
			state,
			index,
			&entry->snapshot.graph->memtable,
			entry->snapshot.recovery,
			initial_source != NULL ? query_terms : &no_terms,
			initial_source != NULL ? query_term_count : 0);
	total_docs = entry->snapshot.graph->metapage.total_docs;
	total_len  = entry->snapshot.graph->metapage.total_len;
	if (source != NULL)
	{
		total_docs += source->total_docs;
		total_len += source->total_len;
		if (initial_source == NULL)
			tp_source_close(source);
	}
	entry->snapshot.total_docs	= Min(total_docs, PG_INT32_MAX);
	entry->snapshot.avg_doc_len = total_docs > 0
										? (float4)((double)total_len /
												   entry->snapshot.total_docs)
										: 0;
	entry->next					= statement->indexes;
	statement->indexes			= entry;
	if (initial_source != NULL)
		*initial_source = source;
	return &entry->snapshot;
}
