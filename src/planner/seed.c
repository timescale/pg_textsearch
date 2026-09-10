/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * seed.c - Bind the filtered-seed top-K to each BM25 index scan
 *
 * The seed depends on a scan's own Limit and its own Filter, both of
 * which are visible on the PlanState tree, and is consumed by tp_rescan,
 * which runs much later: index_beginscan is called lazily from
 * IndexNext on the first tuple fetch, so at executor start there is no
 * IndexScanDesc and no TpScanOpaque to write the seed onto.
 *
 * The one per-scan object that exists in both places is the ORDER BY
 * ScanKey array: ExecIndexBuildScanKeys allocates it during
 * ExecInitIndexScan, and index_rescan forwards the caller's pointer to
 * amrescan uncopied.  So this module walks the PlanState tree at
 * ExecutorStart, computes each BM25 scan's seed, and stores it in a
 * backend-local hash keyed by {ScanKey address, index Oid}.  tp_rescan
 * pulls its own seed back out with the key it was handed.
 *
 * Entries live only as long as the keys they name: the array is
 * allocated in estate->es_query_cxt, and a reset callback on that
 * context removes the entries before its memory is released, so an
 * address cannot be recycled while an entry referencing it is live.
 *
 * A missing entry is never an error: the scan falls back to
 * tp_default_limit and the executor's backoff re-drive, which is what
 * produces the exact top-k in either case.  See
 * docs/issue_435_filtered_seed_scan_identity.md.
 */
#include <postgres.h>

#include <access/skey.h>
#include <executor/executor.h>
#include <limits.h>
#include <miscadmin.h>
#include <nodes/execnodes.h>
#include <nodes/params.h>
#include <nodes/plannodes.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>
#include <utils/rel.h>

#include "index/limit.h"
#include "planner/hooks.h"
#include "planner/seed.h"

static ExecutorStart_hook_type prev_ExecutorStart = NULL;

/*
 * The index Oid is part of the key rather than checked separately so
 * the hash does the comparison: any future violation of the ScanKey
 * address invariant degrades to a miss, not to a wrong seed.
 */
typedef struct TpSeedKey
{
	ScanKey orderbys;
	Oid		index_oid;
} TpSeedKey;

typedef struct TpSeedEntry
{
	TpSeedKey key;
	int		  seed;
} TpSeedEntry;

static HTAB *tp_seed_hash = NULL;

/* Keys bound by one execution, owned by its es_query_cxt. */
typedef struct TpSeedBound
{
	List *keys; /* TpSeedKey * */
} TpSeedBound;

/* State carried across one ExecutorStart's walk. */
typedef struct TpSeedWalkContext
{
	EState		*estate;
	TpSeedBound *bound;		  /* NULL until the first scan is bound */
	Oid			 am_oid;	  /* bm25 access method, lazily resolved */
	bool		 am_resolved; /* whether am_oid has been looked up */
} TpSeedWalkContext;

static void
tp_seed_walk(PlanState *planstate, Limit *limit, TpSeedWalkContext *cxt);

static void
tp_seed_hash_init(void)
{
	HASHCTL ctl;

	if (tp_seed_hash != NULL)
		return;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize	  = sizeof(TpSeedKey);
	ctl.entrysize = sizeof(TpSeedEntry);
	ctl.hcxt	  = TopMemoryContext;

	tp_seed_hash = hash_create(
			"Tapir scan seed bindings",
			16,
			&ctl,
			HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * es_query_cxt reset callback: drop the entries bound by this
 * execution.  Reset callbacks run before the context's memory is
 * released, so the key list is still readable here, and it also fires
 * on error unwind, so a longjmp out of a failed query cannot leak
 * entries.
 *
 * A key recorded here that never reached the hash is harmless --
 * HASH_REMOVE on an absent key is a no-op -- which is what lets the
 * bind record a key before inserting it.
 */
static void
tp_seed_forget(void *arg)
{
	TpSeedBound *bound = (TpSeedBound *)arg;
	ListCell	*lc;

	if (tp_seed_hash == NULL)
		return;

	foreach (lc, bound->keys)
		hash_search(tp_seed_hash, lfirst(lc), HASH_REMOVE, NULL);
}

/*
 * Hand back this execution's key list, creating it and registering its
 * teardown on first use.
 *
 * Registering here rather than after the walk means the callback is
 * always in place before any entry exists, so an error part-way through
 * the walk -- check_stack_depth on a deep plan, or a failed allocation
 * -- cannot strand entries in a hash that outlives the query.  Doing it
 * lazily keeps the cost at zero for the statements that bind nothing,
 * which is nearly all of them.
 */
static TpSeedBound *
tp_seed_bound(TpSeedWalkContext *cxt)
{
	MemoryContextCallback *cb;

	if (cxt->bound == NULL)
	{
		cxt->bound = (TpSeedBound *)palloc0(sizeof(TpSeedBound));
		cb		   = (MemoryContextCallback *)palloc0(sizeof(*cb));
		cb->func   = tp_seed_forget;
		cb->arg	   = cxt->bound;
		MemoryContextRegisterResetCallback(cxt->estate->es_query_cxt, cb);
	}

	return cxt->bound;
}

/*
 * Resolve the bm25 access method Oid, at most once per walk and only
 * once an IndexScan worth checking has been reached: ExecutorStart runs
 * for every statement in the cluster and the first lookup in a backend
 * touches several syscaches.
 */
static Oid
tp_seed_am_oid(TpSeedWalkContext *cxt)
{
	if (!cxt->am_resolved)
	{
		cxt->am_oid		 = tp_get_bm25_am_oid();
		cxt->am_resolved = true;
	}

	return cxt->am_oid;
}

/*
 * Evaluate a Limit expression, if we may.  Core defers this to
 * recompute_limits on the first ExecLimit call, so evaluating it early
 * is our own decision and must be side-effect free.  A PARAM_EXEC comes
 * from an InitPlan that may not have run yet, and a SubPlan
 * (LIMIT (SELECT ...)) would execute a subquery earlier than core
 * intends, so only a Const and an already-bound PARAM_EXTERN qualify.
 */
static bool
tp_seed_eval_limit(Node *expr, EState *estate, int64 *result)
{
	if (expr == NULL)
		return false;

	if (IsA(expr, Const))
	{
		Const *con = (Const *)expr;

		if (con->constisnull || con->consttype != INT8OID)
			return false;

		*result = DatumGetInt64(con->constvalue);
		return true;
	}

	if (IsA(expr, Param))
	{
		Param			*param = (Param *)expr;
		ParamListInfo	 pli   = estate->es_param_list_info;
		ParamExternData *prm;
		ParamExternData	 workspace;

		if (param->paramkind != PARAM_EXTERN || param->paramtype != INT8OID)
			return false;

		if (pli == NULL || param->paramid <= 0 ||
			param->paramid > pli->numParams)
			return false;

		/*
		 * Speculative: a fetch hook must not raise for a value it
		 * cannot supply yet, and an unavailable param just means no
		 * seed.
		 */
		if (pli->paramFetch != NULL)
			prm = pli->paramFetch(pli, param->paramid, true, &workspace);
		else
			prm = &pli->params[param->paramid - 1];

		if (!OidIsValid(prm->ptype) || prm->isnull)
			return false;

		*result = DatumGetInt64(prm->value);
		return true;
	}

	return false;
}

/*
 * k for a Limit node.
 *
 * k is offset + count, matching the planner's own limit_tuples base:
 * reading only the count would seed LIMIT 10 OFFSET 1000 for 10 instead
 * of 1010, a 100x under-seed on exactly the deep-pagination queries
 * where seeding matters most.
 *
 * A sum that reaches INT_MAX yields no k, reproducing the
 * limit_tuples < INT_MAX gate the costing code applied.  Do not clamp
 * to INT_MAX instead: the seed becomes so->limit, which sizes a
 * palloc of k ItemPointerDatas, and LIMIT 2147483647 -- the usual
 * generated-SQL spelling of "no limit" -- would ask for 12 GB.
 *
 * LIMIT ... WITH TIES can return more rows than count, making k a lower
 * bound there; under-seeding falls into backoff, so it needs no special
 * handling.
 */
static bool
tp_seed_limit_k(Limit *limit, EState *estate, int64 *k)
{
	int64 count;
	int64 offset = 0;

	/* No count at all: LIMIT ALL, or an OFFSET-only clause. */
	if (limit->limitCount == NULL)
		return false;

	if (!tp_seed_eval_limit(limit->limitCount, estate, &count))
		return false;

	if (limit->limitOffset != NULL &&
		!tp_seed_eval_limit(limit->limitOffset, estate, &offset))
		return false;

	if (count <= 0)
		return false;

	if (offset < 0)
		offset = 0;

	if (count >= (int64)INT_MAX - offset)
		return false;

	*k = offset + count;

	return true;
}

/*
 * Bind a seed to one BM25 index scan, if the nearest carried Limit
 * applies to it.
 */
static void
tp_seed_bind_scan(IndexScanState *node, Limit *limit, TpSeedWalkContext *cxt)
{
	IndexScan	*plan = (IndexScan *)node->ss.ps.plan;
	Relation	 index_rel;
	Relation	 heap_rel;
	TpSeedBound *bound;
	TpSeedKey	*key;
	TpSeedEntry *entry;
	int64		 k;
	double		 reltuples;
	double		 selectivity;
	int			 seed;

	if (limit == NULL)
		return;

	/*
	 * The conservatism tp_can_pushdown_limit applied on the path, read
	 * off the plan node: exactly one ORDER BY, and no index clauses.
	 * Residual Filter quals are expected -- they are what the
	 * selectivity term below is for.
	 */
	if (list_length(plan->indexorderby) != 1 || plan->indexqual != NIL)
		return;

	if (node->iss_NumOrderByKeys != 1 || node->iss_OrderByKeys == NULL)
		return;

	index_rel = node->iss_RelationDesc;
	heap_rel  = node->ss.ss_currentRelation;
	if (index_rel == NULL || heap_rel == NULL)
		return;

	if (index_rel->rd_rel->relam != tp_seed_am_oid(cxt))
		return;

	if (!tp_seed_limit_k(limit, cxt->estate, &k))
		return;

	/*
	 * Recover the planner's own filter selectivity by division: it set
	 * plan_rows to reltuples * clauselist_selectivity(baserestrictinfo),
	 * and there is no PlannerInfo here to ask again (the selectivity
	 * estimators dereference it).  Both relations are already open on
	 * the scan node, so this costs no syscache lookup and no I/O.
	 *
	 * A scan with no Filter, on a relation that was never analyzed, or
	 * with a degenerate ratio gets no selectivity term -- but it is
	 * still bound, at k alone.  That is the plain LIMIT pushdown, and
	 * binding it per scan is what stops two LIMITs on one index in one
	 * statement from overwriting each other, seeding on or off.
	 */
	reltuples	= heap_rel->rd_rel->reltuples;
	selectivity = 0.0;
	if (plan->scan.plan.qual != NIL && reltuples > 0.0)
		selectivity = plan->scan.plan.plan_rows / reltuples;

	seed = tp_seed_limit_for_filter((int)k, selectivity);

	tp_seed_hash_init();

	key			   = (TpSeedKey *)palloc0(sizeof(TpSeedKey));
	key->orderbys  = node->iss_OrderByKeys;
	key->index_oid = RelationGetRelid(index_rel);

	/* Recorded for teardown before it exists, never after. */
	bound		= tp_seed_bound(cxt);
	bound->keys = lappend(bound->keys, key);

	entry = (TpSeedEntry *)hash_search(tp_seed_hash, key, HASH_ENTER, NULL);
	entry->seed = seed;
}

/*
 * Walk a node's children, excluding its initPlan and subPlan trees:
 * those are separate roots (see tp_bind_scan_seeds) so that an outer
 * Limit cannot leak into a subquery's internal scan.
 *
 * That exclusion is also why Postgres's own planstate_tree_walker is
 * not used here: it walks subplans as children, and it offers no
 * post-visit hook, so there is nowhere to restore the carried Limit on
 * the way back out of a subtree.
 *
 * BitmapAnd and BitmapOr are absent deliberately: their children are
 * only ever bitmap scans, and a BM25 scan is always a plain IndexScan
 * (the opclass declares no search operators, so no bitmap path exists).
 */
static void
tp_seed_walk_children(
		PlanState *planstate, Limit *limit, TpSeedWalkContext *cxt)
{
	tp_seed_walk(outerPlanState(planstate), limit, cxt);
	tp_seed_walk(innerPlanState(planstate), limit, cxt);

	switch (nodeTag(planstate->plan))
	{
	case T_Append:
	{
		AppendState *astate = (AppendState *)planstate;

		for (int i = 0; i < astate->as_nplans; i++)
			tp_seed_walk(astate->appendplans[i], limit, cxt);
		break;
	}

	case T_MergeAppend:
	{
		MergeAppendState *mstate = (MergeAppendState *)planstate;

		for (int i = 0; i < mstate->ms_nplans; i++)
			tp_seed_walk(mstate->mergeplans[i], limit, cxt);
		break;
	}

	case T_SubqueryScan:
		tp_seed_walk(((SubqueryScanState *)planstate)->subplan, limit, cxt);
		break;

	case T_CustomScan:
	{
		ListCell *lc;

		foreach (lc, ((CustomScanState *)planstate)->custom_ps)
			tp_seed_walk((PlanState *)lfirst(lc), limit, cxt);
		break;
	}

	default:
		break;
	}
}

/*
 * Carry the nearest enclosing Limit down to the scans it bounds.
 *
 * A Limit's k may only reach a scan if every node in between preserves
 * row identity and order, because the seed formula assumes "k rows out
 * of the Limit" implies "about k/s rows out of the scan".  The switch is
 * therefore a whitelist: an unrecognized node type -- including any
 * added by a future Postgres -- clears the Limit, and the scan below it
 * falls back to tp_default_limit.
 *
 * Clearing on everything else reproduces the planner's own
 * conservatism, which forces root->limit_tuples to -1 for grouping,
 * DISTINCT, aggregates, window functions and HAVING.  Sort and
 * IncrementalSort clear because a sorted top-k has no relationship to
 * the scan's BM25 top-k; the join nodes clear because k above a join
 * says nothing about how deep either side must go; Gather and
 * GatherMerge clear because parallel BM25 scans are unsupported.
 */
static void
tp_seed_walk(PlanState *planstate, Limit *limit, TpSeedWalkContext *cxt)
{
	if (planstate == NULL)
		return;

	check_stack_depth();

	switch (nodeTag(planstate->plan))
	{
	case T_Limit:
		/* Nearest ancestor wins: overwrite on the way down. */
		limit = (Limit *)planstate->plan;
		break;

	case T_Append:
	case T_MergeAppend:
	case T_SubqueryScan:
	case T_Result:
	case T_LockRows:
		/* Row identity and order preserved; each arm is bounded by k. */
		break;

	case T_IndexScan:
		tp_seed_bind_scan((IndexScanState *)planstate, limit, cxt);
		break;

	default:
		limit = NULL;
		break;
	}

	tp_seed_walk_children(planstate, limit, cxt);
}

static void
tp_bind_scan_seeds(QueryDesc *queryDesc)
{
	EState			 *estate = queryDesc->estate;
	TpSeedWalkContext cxt;
	MemoryContext	  oldcontext;
	ListCell		 *lc;

	if (estate == NULL || estate->es_query_cxt == NULL)
		return;

	oldcontext = MemoryContextSwitchTo(estate->es_query_cxt);

	cxt.estate		= estate;
	cxt.bound		= NULL;
	cxt.am_oid		= InvalidOid;
	cxt.am_resolved = false;

	tp_seed_walk(queryDesc->planstate, NULL, &cxt);

	/*
	 * InitPlan and SubPlan trees are additional roots: they do not hang
	 * off queryDesc->planstate, so without this every BM25 scan inside
	 * a CTE or InitPlan would be skipped.  Starting them with no
	 * carried Limit is also the right answer -- an outer LIMIT must not
	 * leak in, while a LIMIT written inside the subquery appears as a
	 * Limit node within that root and is found normally.
	 */
	foreach (lc, estate->es_subplanstates)
		tp_seed_walk((PlanState *)lfirst(lc), NULL, &cxt);

	MemoryContextSwitchTo(oldcontext);
}

static void
tp_executor_start_hook(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (queryDesc->planstate != NULL)
		tp_bind_scan_seeds(queryDesc);
}

void
tp_seed_hook_init(void)
{
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = tp_executor_start_hook;
}

int
tp_seed_lookup(ScanKey orderbys, Oid index_oid)
{
	TpSeedKey	 key;
	TpSeedEntry *entry;

	if (tp_seed_hash == NULL || orderbys == NULL || !OidIsValid(index_oid))
		return -1;

	memset(&key, 0, sizeof(key));
	key.orderbys  = orderbys;
	key.index_oid = index_oid;

	entry = (TpSeedEntry *)hash_search(tp_seed_hash, &key, HASH_FIND, NULL);
	if (entry == NULL)
		return -1;

	return entry->seed;
}
