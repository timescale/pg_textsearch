# Issue #435: Key the filtered-seed top-K by scan identity

Status: implementation plan, revised after design review.
Issue [#435](https://github.com/timescale/pg_textsearch/issues/435).
Follow-up from PR
[#434](https://github.com/timescale/pg_textsearch/pull/434).

The seed formula does not change. The handoff from planner to
executor does.

---

## 1. The bug

### 1.1 What #434 added

A filtered BM25 top-k query

```sql
SELECT id FROM docs
WHERE facet = 'rare'
ORDER BY body <@> q
LIMIT 10;
```

is planned as a BM25 index scan (internal top-K by score) with the
`WHERE` applied as an executor **Filter** above it. If the scan
produces only the top `k` by score, few may pass the Filter, and
the executor **re-drives** the scan with a doubled internal limit
until `k` survivors exist. Each re-drive re-scores from scratch.

#434 seeds the internal top-K at plan time so one scoring pass
usually suffices:

```
seed = ceil(margin * k / s)      capped at TP_MAX_QUERY_LIMIT
```

`margin` is `pg_textsearch.filtered_seed_margin` (default 3.0).
`s` is `clauselist_selectivity` over the scanned relation's
`baserestrictinfo`. The seed only changes scan depth; Filter +
Limit + backoff still decide which rows win, so results are
identical to the un-seeded plan.

### 1.2 The handoff, today

`tp_costestimate` (`src/planner/cost.c`) computes the seed and
calls `tp_store_query_limit(index_oid, seeded)`. The stash
(`src/index/limit.c`) is one static slot keyed only by
`index_oid`:

```c
static TpCurrentLimit tp_current_limit = {InvalidOid, -1, false};
```

`tp_rescan` reads it via `tp_get_query_limit(scan->indexRelation)`
and clears it. A miss leaves `so->limit = -1`, and scoring falls
back to `tp_default_limit` (1000).

### 1.3 Failure shape

```sql
SELECT id FROM docs WHERE facet = 'rare'   ORDER BY body <@> q LIMIT 10
UNION ALL
SELECT id FROM docs WHERE facet = 'common' ORDER BY body <@> q LIMIT 10;
```

Both arms scan `docs_body_idx`. With `s = 0.01` for `rare`,
`s = 0.5` for `common`, and margin 3.0:

```
PLAN   tp_costestimate(rare)   -> store(idx, 3000)   slot = 3000
       tp_costestimate(common) -> store(idx,   60)   slot =   60   <- overwrote
       (also fires for discarded paths, any number of times)

EXEC   rare   tp_rescan -> takes 60, clears slot     rare   runs at   60
       common tp_rescan -> slot empty -> -1          common runs at 1000
```

Neither arm receives its own seed. The same collision occurs for a
self-join, a CTE referenced twice, and for two different LIMITs on
one index (which already collided before #434). `amcostestimate`
is also not 1:1 with chosen scans: it runs for discarded paths and
may run several times for the chosen one.

### 1.4 Impact

**Performance only.** The Filter, the Limit, and the backoff
re-drive still produce the exact top-k. A mis-seeded arm either
over-scores (bounded by `TP_MAX_QUERY_LIMIT`) or falls into the
backoff loop the seed was meant to avoid.

Backoff is why any seed is safe: when the batch is exhausted
(`current_pos >= result_count`) **and** it was full
(`result_count >= max_results_used`), scoring stopped at the cap
rather than at the end of the corpus, so `tp_gettuple` doubles
`so->limit` and re-scores. Emitted CTIDs are deduped, so a
re-score cannot return a row twice.

---

## 2. Goals and non-goals

### Goals

1. Each BM25 index scan gets the seed computed from **its** Limit
   and **its** Filter, including two scans of one `index_oid`.
2. Results remain identical to `filtered_seed = off`.
3. Nested-loop re-scans of a node reuse that node's seed.
4. Prepared / generic / cached plans work. The seed must not live
   in a backend-local map filled during `planner_hook`, which does
   not run on `EXECUTE` of a cached plan.
5. Remove the single-slot side channel from `amcostestimate`.
   Costing is not "this path was chosen" and has no scan identity.

### Non-goals

- Changing the seed formula, GUC names, defaults, or ranges.
- Filtering during scoring (#434 rejected an allow-list of heap
  CTIDs as slower than seeding).
- Making `amcostestimate` cost figures use the seeded depth.
- CustomScan wrapping or a new field on `IndexScan`.
- Parallel BM25 index scans (not supported today).

---

## 3. What exists when

This table is the whole design constraint:

```
                         Plan  PlanState  OrderByKeys  ScanDesc + so
  planner_hook             y        -           -            -
  ExecutorStart_hook       y        y           y            -
  IndexNext (1st fetch)    y        y           y            y   <- here
```

The seed is knowable in the top two rows, where the Limit and the
Filter are visible. The AM lives in the bottom row.

**`index_beginscan` is called lazily from `IndexNext` on the first
tuple fetch, not from `ExecInitIndexScan`.** Verified in
`nodeIndexscan.c` on both `REL_17_STABLE` and `REL_18_STABLE`: the
only non-parallel `index_beginscan` call sites are inside
`IndexNext` and `IndexNextWithReorder`, each guarded by
`if (scandesc == NULL)`. So at `ExecutorStart` time
`iss_ScanDesc` is NULL for every scan node and `TpScanOpaque`
does not exist yet.

This is not a recent change and is not going to revert. PG 9.6
created the descriptor eagerly in `ExecInitIndexScan`; PG10 moved
it — together with the initial `index_rescan` — into `IndexNext`
for parallel index scans, with the reason in the comment there:
"We reach here if the index scan is not parallel, or if we're
serially executing an index scan that was planned to be
parallel." There are three mutually exclusive creation sites
(`index_beginscan` for serial, and `index_beginscan_parallel`
from `ExecIndexScanInitializeDSM` and
`ExecIndexScanInitializeWorker`), and which applies is unknown at
init time, because `Gather` decides at execution time how many
workers it can launch and the leader runs the plan serially if it
gets none.

The ScanKey arrays sit on the **eager** side of that boundary for
a structural reason: they are per-node executor state that all
three creation paths must pass to `amrescan`, so they are
independent of parallelism. The parallel paths pass the same
`node->iss_OrderByKeys`. Keying on it is therefore not a bet on
an accident of the current implementation.

`IndexScanDescData` has no back-pointer to the `PlanState`, so the
AM cannot pull the value either. The only field pointing into the
node is `instrument` (PG18 only, NULL without instrumentation),
which would need a containerof hack.

`ExecIndexBuildScanKeys` allocates `iss_OrderByKeys` during
`ExecInitIndexScan` — after the `EXEC_FLAG_EXPLAIN_ONLY` early
return, before any `index_beginscan`. That array is the only
per-scan object present both where the seed is knowable and where
the AM asks for it. It is therefore the key.

---

## 4. Chosen design

Bind at `ExecutorStart`, keyed by the scan node's ORDER BY ScanKey
array; look up in `tp_rescan`.

```
ExecutorStart_hook   (after standard_ExecutorStart)
   walk planstate + es_subplanstates, carrying nearest valid Limit
   for each BM25 IndexScanState:
       k    = offset + count from that Limit
       s    = plan->plan_rows / reltuples
       seed = tp_seed_limit_for_filter(k, s)
       hash[{node->iss_OrderByKeys, indexid}] = seed

IndexNext (first fetch)
   index_beginscan -> tp_beginscan          so->limit = -1
   index_rescan(..., orderbys, norderbys)
       -> tp_rescan: lookup {orderbys, RelationGetRelid(...)}
                     hit  -> so->limit = seed
                     miss -> so->limit = -1  (tp_default_limit)
   index_getnext_slot -> tp_gettuple        scores at so->limit

later index_rescan (nested loop)
   -> same lookup, so->limit restored to seed
```

For the §1.3 example:

```
ExecutorStart_hook
   rare   node: OrderByKeys=0xA0, Limit 10, s=0.01 -> hash[{0xA0,idx}]=3000
   common node: OrderByKeys=0xC8, Limit 10, s=0.5  -> hash[{0xC8,idx}]=  60

ExecProcNode(rare)   -> index_rescan(..., 0xA0) -> lookup -> 3000
ExecProcNode(common) -> index_rescan(..., 0xC8) -> lookup ->   60
```

The mechanism **pulls** ("what is *my* seed?") rather than
**pushes** ("here is a seed for whoever asks next"). Execution
order stops mattering, a nested InitPlan scan cannot steal another
scan's seed because it asks with its own key, and a nested-loop
rescan restores the seed by repeating the lookup.

### 4.1 The key

Composite: `{ScanKey address, index Oid}`.

`index_rescan` forwards the caller's `orderbys` pointer to
`amrescan` with no copy, so `tp_rescan`'s `orderbys` parameter is
literally `node->iss_OrderByKeys`.

The key is the array's **address**, not its contents, so two scans
with byte-identical `bm25query` values still get distinct keys.
There is exactly one `ExecIndexBuildScanKeys` call per
`IndexScanState` and one `IndexScanState` per plan node per
`EState`, so arrays are one-per-node by construction.

The index Oid is in the key rather than checked separately so the
hash does the comparison. It is defence in depth: any future
violation of the address invariant degrades to a miss, not to a
wrong seed.

### 4.2 Lifetime

`ExecIndexBuildScanKeys` pallocs in the current context during
`ExecInitNode`, which is `estate->es_query_cxt`. The walk collects
the keys it inserted into a list allocated in that same context
and registers a `MemoryContextCallback` on it; the callback
removes those entries.

Context reset callbacks run **before** the context's memory is
released, so reading the key list inside the callback is safe, and
**an address cannot be recycled while an entry referencing it is
live**. That closes the stale-pointer window structurally rather
than probabilistically. The callback also fires on error unwind,
so a longjmp out of a failed query cannot leak entries.

Nested executions are safe without a stack: the outer and inner
`PlanState` trees are alive simultaneously, so their ScanKey
arrays are at distinct addresses.

### 4.3 The invariant

**A missing key or a missing entry always means `tp_default_limit`
plus backoff — never an error, never a wrong answer.**

Every degradation path collapses to that single fallback, which is
exactly today's behavior on a missed stash:

- no ORDER BY, so `norderbys == 0` and there is no key;
- ORDER BY but no LIMIT ancestor;
- a plan shape the whitelist in §6 does not recognize;
- a LIMIT expression we decline to evaluate (§5.1);
- `reltuples <= 0`, or a degenerate selectivity ratio;
- a non-executor `index_beginscan` caller;
- an `EvalPlanQual` re-evaluation (see §7);
- a recycled address caught by the Oid half of the key.

This is why the design does not depend on planner behavior we do
not control. In particular, do **not** justify anything by "a
BM25 scan always has an ORDER BY" — see §7.

### 4.4 No `planned_limit` field

Because `tp_rescan` re-looks-up on every call, `TpScanOpaque`
needs no new field. Nested-loop rescans restore the seed for free,
and backoff's writes to `so->limit` are naturally transient: the
next rescan resets to the seed, which is correct, since a new
execution of the scan should start from the seed rather than a
leftover doubled cap.

---

## 5. Seed computation at bind time

The formula moves to `src/index/limit.c` as a pure function:

```c
int tp_seed_limit_for_filter(int user_limit, double selectivity);
```

Same rules as `cost.c` has today:

- `filtered_seed` off -> return `user_limit`.
- `selectivity <= 0` or `>= 1` -> return `user_limit`.
- `seed = ceil(margin * user_limit / selectivity)`.
- cap at `TP_MAX_QUERY_LIMIT`.
- if `seed <= user_limit` -> return `user_limit`.

It takes a plain `double` rather than `Selectivity` so `limit.c`
needs no planner headers; the GUCs it reads are already declared
in `constants.h`, which it includes.

### 5.1 `k` from the nearest valid Limit

**`k` is offset + count, not count.** The planner's own base is
the sum:

```c
		if (count_est > 0 && offset_est >= 0)
			limit_tuples = (double) count_est + (double) offset_est;
```

Reading only `limitCount` would seed `LIMIT 10 OFFSET 1000` for
k=10 instead of 1010 — a 100x under-seed on exactly the deep
pagination queries where seeding matters most. Compute the sum in
`int64`; the two terms can overflow.

**A sum that reaches `INT_MAX` yields no `k`.** Do not clamp to
`INT_MAX` — the seed becomes `so->limit`, which sizes a `palloc`
of `k` `ItemPointerData`s, so `LIMIT 2147483647` would ask for
12 GB on a query that works today. Skipping reproduces the
costing gate this replaces, which was `limit_tuples > 0 &&
limit_tuples < INT_MAX`. See §13 for the underlying allocation
bug, which is out of scope here.

**Only `Const` and `PARAM_EXTERN` may be evaluated.** Core defers
limit evaluation to `recompute_limits` on the first `ExecLimit`
call, so evaluating early is our own decision and must be
side-effect free:

- `Const` integer -> use it.
- `PARAM_EXTERN` -> evaluate; these are bound on the `EState`
  before execution starts. This is the one case where binding at
  executor start beats plan time: `LIMIT $1` on a generic plan
  gets a real `k`, where today `preprocess_limit` cannot fold the
  Param and leaves `root->limit_tuples` at -1.
- `PARAM_EXEC` -> **skip**. Its value comes from an InitPlan that
  may not have run at bind time.
- anything else, including a SubPlan from `LIMIT (SELECT ...)`
  -> **skip**. Evaluating it would execute a subplan earlier than
  core intends.
- `noCount` / `LIMIT ALL` / missing -> skip.

`LIMIT ... WITH TIES` can return more rows than `count`, so `k` is
a lower bound there. Under-seeding falls into backoff, so no
special handling is needed.

### 5.2 `s` from the plan node

There is no `PlannerInfo` at executor start, and
`restriction_selectivity()` is **not** a workaround: it takes
`PlannerInfo *root` (`optimizer/plancat.h`) and the estimators
behind it dereference it — `eqsel` reaches
`examine_variable` -> `find_base_rel(root, varno)` ->
`root->simple_rel_array`. Passing NULL segfaults.

Instead, read back the planner's own answer. `plan_rows` on the
scan node is `rel->rows`, and the planner computes that with the
identical call the current seeding code makes:

```c
	nrows = rel->tuples *
		clauselist_selectivity(root, rel->baserestrictinfo, 0,
							   JOIN_INNER, NULL);
	rel->rows = clamp_row_est(nrows);
```

So:

```
    s = plan->plan_rows / reltuples          clamped to (0, 1]
```

with `plan_rows` from `ss.ps.plan->plan_rows` and `reltuples` from
`ss_currentRelation->rd_rel->reltuples` — the heap is already open
on the scan node, so this is O(1) with no syscache and no I/O.

This is not a weaker estimate; it is the same number #434
computes, recovered by division, including any extended statistics
the planner used. The #434 concern about
`restriction_selectivity` disagreeing on correlated quals does not
arise.

These guards drop the selectivity term only. They do **not** skip
the entry: once `k` is known the scan is always bound, at `k`
alone, and `tp_seed_limit_for_filter` returns `user_limit`
unchanged for a degenerate `s`.

- `plan->qual == NIL` -> no Filter, nothing to seed for;
- `reltuples <= 0` -> never analyzed. `CREATE INDEX` sets it via
  `index_update_stats`, so this is rare in practice;
- computed `s >= 1` -> degenerate, no seeding.

Binding at `k` in these cases is required, not incidental. It is
the plain LIMIT pushdown that predates #434, and §7 and §12 both
depend on it: it is what stops two LIMITs on one index in one
statement from overwriting each other with `filtered_seed = off`.
Landing these guards on §4.3 instead would regress an unfiltered
`ORDER BY score LIMIT 10` from an internal limit of 10 to
`tp_default_limit`, scoring 100x deeper. Only a missing `k`
(§5.1) lands on §4.3.

Known imprecision, accepted: the planner uses `estimate_rel_size`,
which scales `reltuples` by current block count over `relpages`,
so on a table that grew since ANALYZE our recovered `s` is
proportionally too large and we under-seed into backoff.
Shrinkage over-seeds, bounded by the cap. Both are benign.
Replicating the density scaling would cost an `smgr` size call per
BM25 scan at ExecutorStart; not worth it.

A qual on an intervening `Result` is not reflected in the scan's
`plan_rows`, so we under-seed there. Using the `plan_rows` of the
node just beneath the Limit would capture it but breaks for
`Append`, where the sum across arms is the wrong per-arm
denominator. Left as-is; backoff covers it.

### 5.3 Executor-side pushdown conservatism

`tp_can_pushdown_limit` checked, on the path: exactly one ORDER BY,
and no index clauses. On the plan node those become

- `list_length(indexscan->indexorderby) == 1`
- `indexscan->indexqual == NIL`

Residual Filter quals are expected and are the reason we seed.

### 5.4 Identifying a BM25 scan

`IsA(plan, IndexScan)` plus `relam == bm25_am_oid`, read from the
already-open `iss_RelationDesc->rd_rel->relam` — no syscache.

Look up the AM Oid **lazily**, on the first `IndexScan` node the
walk encounters, not at the top of the hook. `ExecutorStart_hook`
runs for every statement in the cluster, and the first
`get_bm25_oids()` call in a backend does a `TypenameGetTypid` plus
four `OpernameGetOprid` lookups. Do not pay that on non-BM25
statements.

BM25 paths require ORDER BY and a heap fetch for the Filter, so
they are `IndexScan`, never `IndexOnlyScan` or `BitmapIndexScan`.
`amcanreturn` is NULL, so index-only scans are impossible.

---

## 6. The PlanState walk

The walk carries a "current Limit" downward. **A Limit's `k` may
only reach a scan if every node in between preserves row identity
and order**, because the formula assumes "k rows out of the Limit"
implies "about k/s rows out of the scan."

Written as a **whitelist**, so an unrecognized node type —
including any added by a future Postgres — clears the Limit and
the scan falls back to the default:

| Node | Effect |
| --- | --- |
| `T_Limit` | set current Limit, then walk child |
| `T_Append`, `T_MergeAppend` | pass through; each arm is bounded by `k` |
| `T_SubqueryScan`, `T_Result`, `T_LockRows` | pass through |
| `T_IndexScan` | candidate; bind if BM25 and §5.3 holds |
| anything else | **clear** the current Limit, then walk children |

Because the walk overwrites on the way down, the nearest ancestor
wins: `SELECT * FROM (SELECT ... LIMIT 100) LIMIT 10` seeds for
100.

Clearing on everything else is what reproduces the planner's own
conservatism. It forces `root->limit_tuples` to -1 when the query
has grouping, grouping sets, `DISTINCT`, aggregates, window
functions, target-list SRFs, or `HAVING`; clearing on `Agg`,
`Group`, `WindowAgg`, `SetOp`, `Unique` and `ProjectSet` gets the
same behavior structurally. `Sort` and `IncrementalSort` clear
because a sorted top-k has no relationship to the scan's BM25
top-k. `Gather` and `GatherMerge` clear, consistent with parallel
BM25 scans being unsupported.

### 6.1 Joins are deliberately excluded

`NestLoop`, `HashJoin` and `MergeJoin` are not on the whitelist,
so a BM25 scan beneath a join is no longer seeded. Today it is,
because `root->limit_tuples` applies to the whole subquery. This
is an intentional change; record it in §11.

The reason is the whitelist's own principle: the formula assumes
"k rows out of the Limit" implies "about k/s rows out of the
scan," and a join breaks that implication by multiplying and
filtering rows between the two. `k` above a join says nothing
about how deep either side must go, so there is no defensible
value to seed with.

Note that "a nested-loop inner is re-driven per outer row, so a
deep seed multiplies" is **not** a valid argument, and earlier
revisions of this document used it. It fails twice:

1. A BM25 scan can never be a *parameterized* nestloop inner.
   Parameterization needs join clauses usable as index quals, and
   the opclass declares only `OPERATOR 1 <@> ... FOR ORDER BY`
   with no search operators, so `indexclauses` is always NIL (the
   same fact §7.1 relies on). An unparameterized inner is
   possible, but `match_unsorted_outer` always offers a
   `create_material_path` alternative that competes on cost, and
   Material rewinds a tuplestore instead of rescanning — it wins
   precisely when the child is expensive.
2. A deep seed is not wasted work. For 10k evaluations at
   `s = 0.01` and margin 3: with `LIMIT 1` the seed is 300 versus
   a 1000 default, so seeding is *cheaper* per iteration. With
   `LIMIT 10` the seed is 3000, but the unseeded run scores
   1000 + 2000 + 4000 = 7000 documents backing off to a
   comparable depth. Depth is what surfaces `k` survivors;
   scanning shallower just means re-scoring.

### 6.2 Subplans are separate roots, and are seeded

InitPlan and SubPlan trees live in `estate->es_subplanstates`, not
under `queryDesc->planstate`. The walk must take each as an
additional root, or it silently skips every BM25 scan inside a CTE
or InitPlan.

Walking them as roots also gives the right answer for free: with
no carried Limit, an outer `LIMIT 10` cannot leak into a CTE's
internal scan, while a `LIMIT` written inside the CTE appears as a
Limit node within that root and is found normally.

A **correlated** SubPlan is therefore seeded, and its scan is
re-driven once per evaluation — `ExecScanSubPlan` calls
`ExecReScan(planstate)` every time. That is deliberate: the
premise of §6.1 holds here, because the Limit is inside the
subquery, directly above the scan, so `k` really is the number of
rows that evaluation needs.

The residual risk is over-seeding from a bad `s`, paid once per
evaluation rather than once per statement. It is bounded by
`TP_MAX_QUERY_LIMIT` and it exists for single scans too, just
without the multiplier. If it ever shows up in practice, the fix
is a better `s`, not refusing to seed.

For completeness, the other per-iteration rescan drivers —
`RecursiveUnion`'s recursive term and a `Memoize` child — are not
on the whitelist, so they clear the Limit and go unseeded.

### 6.3 Rescans are trigger-agnostic

Every rescan path reaches the AM through `ExecReScanIndexScan`,
which always passes `node->iss_OrderByKeys`:

```c
	if (node->iss_ScanDesc)
		index_rescan(node->iss_ScanDesc,
					 node->iss_ScanKeys, node->iss_NumScanKeys,
					 node->iss_OrderByKeys, node->iss_NumOrderByKeys);
```

So it does not matter what caused the rescan — a nestloop outer
tuple, a SubPlan evaluation, a `RecursiveUnion` iteration, a
`Memoize` miss, a propagated `ExecReScan` from any ancestor, a
`chgParam` change, or an `ExecutorRewind` on a cursor. The scan
asks with its own key and gets its own seed back. The only
exception is the EPQ tree (§7).

One wrinkle: ORDER BY expressions share `iss_RuntimeKeys` with
index quals, so a non-Const query such as
`to_bm25query($1, 'idx')` makes `iss_NumRuntimeKeys > 0` and
`ExecIndexScan` calls `ExecReScan` on its first invocation. That
is harmless — `iss_ScanDesc` is still NULL, so the `if` above
skips `index_rescan`, and `IndexNext` then does beginscan plus
rescan as usual.

### 6.4 Hook registration

```c
static ExecutorStart_hook_type prev_ExecutorStart = NULL;

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
```

`ExecutorStart_hook_type` returns `void` in both PG17 and PG18
(checked in `executor/executor.h` for both).

Do **not** gate on `query_has_bm25_operators`. That flag is set in
`post_parse_analyze` and is not reliable for a cached plan.

---

## 7. Edge cases

| Case | Behavior |
| --- | --- |
| Single filtered top-k (the #434 path) | One Limit, one scan, one seed. |
| No Filter (`qual == NIL`) | `user_limit` only; helper is a no-op. |
| `filtered_seed = off` | Helper returns `user_limit`. Still per-scan, so two LIMITs on one index each get their own `k` — a fix even with seeding off. |
| Two LIMITs, same index, no filters | Each scan gets its own `k`. Fixes the pre-#434 collision. |
| UNION ALL, two filters, same `k` | Each arm gets its own seed. The #435 example. |
| Limit above Append | Same `k`, different `s` per arm. |
| Self-join / CTE inlined twice | Two `IndexScanState`s, two ScanKey arrays, two entries. |
| CTE materialized | One scan, one entry. Correct: only one scan runs. |
| Two subqueries, identical `bm25query` | Distinct addresses, distinct entries. |
| Nested-loop rescan of a seeded node | Same lookup, seed restored. |
| BM25 scan beneath a join | Not seeded (§6.1). |
| BM25 scan in a correlated SubPlan | Seeded; re-driven once per evaluation, seed restored by each rescan (§6.2). |
| BM25 scan in a `RecursiveUnion` term or under `Memoize` | Limit cleared, unseeded. |
| Backoff after bind | Doubles `so->limit`; next rescan resets to the seed. |
| Prepared / generic plan | Bind runs every `ExecutorStart`; `LIMIT $1` evaluated then. |
| Intervening Sort / Agg / Unique | Limit cleared, no entry, default limit. |
| `EvalPlanQual` re-evaluation | `EvalPlanQualStart` calls `ExecInitNode` directly rather than `ExecutorStart`, so the EPQ tree gets a fresh ScanKey array and no entry. Unseeded, correct. |
| `EXEC_FLAG_EXPLAIN_ONLY` | `ExecInitIndexScan` returns before building order-by keys; no entry, nothing executes. |
| Non-executor `index_beginscan` | No entry, default limit. |
| `amcostestimate` on discarded paths | No effect; nothing is stored from costing. |

### 7.1 Out of scope: no-ORDER-BY BM25 scans

A BM25 index scan with no ORDER BY is not impossible, only
unlikely, so §4.3 must not lean on it. Two gates:

1. `build_index_paths` creates a path if there are index clauses,
   useful pathkeys, a **useful predicate**, or an index-only scan.
   Index clauses can never apply (the opclass declares only
   `OPERATOR 1 <@> ... FOR ORDER BY`, no search operators),
   index-only scans need `amcanreturn` (NULL). But a **partial**
   BM25 index whose predicate the query implies makes
   `useful_predicate` true, with no ORDER BY involved.
2. `tp_costestimate` returns infinite cost in that case, which
   loses to the seq scan — airtight on PG17. PG18 added
   `disabled_nodes` to `Path` and compares it before cost, so with
   `enable_seqscan = off` a disabled seq scan loses to a
   zero-disabled-nodes index path whose infinite cost is never
   consulted.

Under §4.3 that shape is harmless for seeding: `norderbys == 0`,
no key, default limit.

Separately and **not part of this work**: such a scan has no query
text, and `tp_gettuple` guards that only with
`Assert(so->query_text != NULL)`, so a non-assert build takes the
`!so->query_text` early return in `tp_execute_scoring_query` and
yields zero rows silently. That would be a wrong answer rather
than a slow one. The code paths have been read but the behavior
has not been reproduced. Needs its own issue and a repro attempt:
PG18, partial BM25 index, query implying the predicate,
`enable_seqscan = off`, no ORDER BY.

---

## 8. File-by-file changes

### `src/planner/seed.c`, `src/planner/seed.h` (new)

The `ExecutorStart_hook`, the `PlanState` walk, the hash, and the
reset callback. New files rather than growing `hooks.c`, which is
already past 2000 lines.

Exports `tp_seed_hook_init(void)` for `_PG_init` and
`int tp_seed_lookup(ScanKey orderbys, Oid index_oid)` for
`access/scan.c` (returns -1 on miss). Reuses `BM25OidCache` /
`get_bm25_oids()` from `hooks.c`, called lazily per §5.4.

Add `src/planner/seed.o` to `OBJS` in the Makefile.

### `src/index/limit.h`, `src/index/limit.c`

Add `tp_seed_limit_for_filter(int user_limit, double selectivity)`.

Delete `TpCurrentLimit`, `tp_store_query_limit`,
`tp_get_query_limit`, `tp_cleanup_query_limits` (declared, never
called anywhere), and `tp_can_pushdown_limit` (unreachable once
`cost.c` stops calling it; its conditions move to §5.3).

`tp_store_query_limit` also contains an `if` with an empty body,
which goes with it. Drop the `<access/xact.h>`,
`<nodes/pathnodes.h>` and `<utils/rel.h>` includes; add
`<math.h>`. Keep `tp_default_limit`.

Update the file comment: this module is the seed formula and the
default limit, not a planner-to-executor slot.

### `src/planner/cost.c`

Delete the static `tp_seed_limit_for_filter` and the entire

```c
	if (root && root->limit_tuples > 0 && root->limit_tuples < INT_MAX)
	{ ... tp_can_pushdown_limit ... tp_store_query_limit ... }
```

block, including the #435 NOTE comment. Its explanatory comment
about why seeding exists moves to `limit.c` with the helper.

Keep the later `indexSelectivity` block: it reads
`root->limit_tuples` directly, never called
`tp_can_pushdown_limit`, and is genuinely about costing. Drop the
now-unused `index/limit.h` and `<math.h>` includes.

Nothing is published from costing.

### `src/access/scan.c`

In `tp_rescan`, replace

```c
		int query_limit = tp_get_query_limit(scan->indexRelation);
		so->limit		= (query_limit > 0) ? query_limit : -1;
```

with a lookup keyed by the `orderbys` parameter and
`RelationGetRelid(scan->indexRelation)`, guarded on
`norderbys > 0 && orderbys != NULL`. No new `TpScanOpaque` field
(§4.4).

Add the scoring-pass counter increment in
`tp_execute_scoring_query` (§9.2).

### `src/mod.c`

Call `tp_seed_hook_init()` from `_PG_init`. No GUC changes.

### `sql/pg_textsearch--1.5.0-dev.sql`

`bm25_debug_scoring_passes(reset boolean DEFAULT false)`
returning `bigint` (§9.2).

### Docs

- `CLAUDE.md` / `AGENTS.md` (kept in sync): one line that the seed
  is bound per scan at executor start, keyed by scan identity, not
  via a per-`index_oid` slot.
- `bm25_debug_scoring_passes` in the debug-functions list.
- This file is the design record.

---

## 9. Testing

### 9.1 Correctness parity

Extend `test/sql/filtered_seed.sql`, keeping the existing parity /
oracle / no-op / margin / GUC-range cases and reusing `fs_docs`
and `fs_check`. Compare as sets (`array_agg ORDER BY id`).

1. **UNION ALL, two facets, same LIMIT** — the issue example, each
   arm compared against the same arm with `filtered_seed = off`.

   ```sql
   SELECT id FROM fs_docs WHERE facet_id = 6
   ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10
   UNION ALL
   SELECT id FROM fs_docs WHERE facet_id = 13
   ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10;
   ```

2. **UNION ALL, two different LIMITs.**
3. **Self-join** with two different facet predicates.
4. **CTE referenced twice.**
5. **Unfiltered UNION ALL**, `LIMIT 10` and `LIMIT 50` on one
   index; each arm equals the standalone query with that LIMIT.
6. **PREPARE / EXECUTE** of (1) six or more times, so the plan
   flips from custom to generic, with and without changing
   `filtered_seed_margin` between executes.
7. **`LIMIT 10 OFFSET 20`** parity, covering §5.1.
8. **Intervening Sort** (`ORDER BY id` above a BM25 subquery) and
   **Agg** (`count(*)` over a limited subquery): results correct,
   simply unseeded.

### 9.2 Depth, which is what actually proves the fix

The §9.1 cases all pass on main, because backoff hides a stolen
seed. Correctness tests cannot fail on a performance-only bug, so
without a depth signal this PR is unverifiable and §12 is
unfalsifiable.

`log_bmw_stats` does not work for this: it prints doc counts and
skip percentages, which are not regression-stable.

Add a session-local counter of scoring passes, incremented in
`tp_execute_scoring_query`, exposed as

```sql
bm25_debug_scoring_passes(reset boolean DEFAULT false) RETURNS bigint
```

A well-seeded arm costs exactly one pass; an under-seeded arm
costs `1 + log2(...)`. So the UNION ALL case asserts **two**
passes total, and that assertion fails on main and passes on the
branch. This is the acceptance test.

This overrules the previous revision's "do not add a user-facing
function solely for this if logging suffices" — logging does not
suffice.

### 9.3 Commands

```bash
make
make installcheck
make format-check
```

Update `test/expected/filtered_seed.out` if needed and re-run.
Every failure matters; do not dismiss any as pre-existing.

---

## 10. Implementation sequence

1. **Extract the formula.** Add
   `tp_seed_limit_for_filter(int, double)` to `limit.c`; have
   `cost.c` call it instead of its local copy. No behavior change.
   `make installcheck`.
2. **Add the scoring-pass counter** and
   `bm25_debug_scoring_passes`. Add the §9.1 tests and record the
   pass counts observed on main — this is the "fails before"
   baseline.
3. **Add `src/planner/seed.c`**: hook, walk, hash, reset callback.
   Switch `tp_rescan` to the lookup and delete
   `tp_store_query_limit` / `tp_get_query_limit` /
   `TpCurrentLimit` / `tp_can_pushdown_limit` and the costing
   block in the same commit. Steps 2 and 3 of the previous
   revision claimed a both-paths interim state would "stay
   identical"; it would not, since the bind runs after the rescan
   and would win. Do it in one step.
4. **Confirm** the §9.2 assertion now holds, and that §9.1 stays
   green.
5. **Format, comments, status line.**

---

## 11. Compatibility and risk

- **On-disk format / WAL / replication:** unchanged. The seed is
  executor-local and never persisted.
- **Types:** unchanged. `bm25query` / `TpQuery` layout untouched.
- **GUCs:** unchanged.
- **Score / ranking:** unchanged. Same backoff safety net as #434.
- **Behavior change:** a BM25 scan beneath a join is no longer
  seeded (§6.1). Deliberate: `k` above a join does not imply
  "about k/s rows from this side," so there is no defensible seed
  value. Unseeded means default plus backoff, which is correct.
- **Unchanged:** a BM25 scan inside a correlated SubPlan is still
  seeded, and still re-driven once per evaluation (§6.2). The
  over-seed risk is paid per evaluation, bounded by the cap.
- **Improvement:** `LIMIT $1` on a generic plan now seeds (§5.1).
- **Improvement:** two different LIMITs on one index in one
  statement no longer overwrite each other, even with
  `filtered_seed = off`.
- **Unchanged:** a `LIMIT` whose `offset + count` reaches
  `INT_MAX` is not seeded, matching the `limit_tuples < INT_MAX`
  costing gate (§5.1). Clamping instead would turn
  `LIMIT 2147483647` into a 12 GB allocation request; see §13.
- **Risk:** the walk misses a node type -> that scan uses
  `tp_default_limit`. Same class as a missed stash today, and the
  whitelist makes it the default for anything unrecognized.
- **Risk:** `plan_rows / reltuples` skews on stale statistics ->
  slightly wrong depth. Bounded by the cap and by backoff (§5.2).
- **Risk:** the ScanKey-address key depends on `index_rescan`
  forwarding `orderbys` to `amrescan` uncopied. Undocumented but
  stable across 17 and 18, and the Oid half of the key makes any
  future change degrade to a miss rather than a wrong seed.
- **Performance:** `ExecutorStart_hook` adds an O(plan nodes) walk
  to every statement in the cluster, with no syscache work on
  non-BM25 plans (§5.4). Worth a sanity check on a non-BM25
  pgbench run.

---

## 12. Success criteria

- The #435 UNION ALL example: each arm's internal top-K derives
  from that arm's Filter and Limit, demonstrated by
  `bm25_debug_scoring_passes` reporting one pass per arm, where
  main reports more.
- Two different LIMITs on one index in one statement no longer
  overwrite each other, even with `filtered_seed = off`.
- Existing `filtered_seed` parity / oracle / no-op / margin tests
  still pass.
- No planner-to-executor global slot remains. The binding hash is
  executor-scoped, keyed by scan identity, and torn down by an
  `es_query_cxt` reset callback; a miss is always the documented
  fallback of §4.3.
- Cached plans, nested-loop rescans, and CTE / InitPlan scans all
  keep the correct per-scan seed.

---

## 13. Follow-up: `so->limit` sizes an unbounded allocation

**Not this change's job. File as its own issue.**

`so->limit` is used directly as an allocation count:

```c
	so->result_ctids = palloc(max_results * sizeof(ItemPointerData));
```

(`src/memtable/scan.c`, where `max_results` is `so->limit` when
positive.) Nothing caps it, so a large user `LIMIT` becomes a large
`palloc` and the query dies with `invalid memory alloc request
size`. Measured on `main`, filtered top-k with
`enable_seqscan = off`: `LIMIT 10000000` works, `LIMIT 200000000`
fails asking for 1.2 GB.

This predates the per-scan binding, and it is why §5.1 refuses a
`k` that reaches `INT_MAX` rather than clamping to it. Clamping
would have bound `INT_MAX` and made `LIMIT 2147483647` — the usual
generated-SQL spelling of "no limit" — request 12 GB, on a query
that works on `main` today because the costing gate was
`limit_tuples < INT_MAX`. The guard restores exact parity; it does
not fix the underlying allocation.

**The obvious fix is wrong.** Capping `so->limit` at
`TP_MAX_QUERY_LIMIT` silently truncates: an unfiltered
`ORDER BY score LIMIT 500000` over a corpus with 500000 matches
would return 100000 rows and stop, because the backoff also
refuses to grow past `TP_MAX_QUERY_LIMIT` and so cannot recover
the difference. A correct fix has to stop pre-allocating `k` slots
up front — grow the result array, or size it from the top-k heap's
actual occupancy — which is a change to the scoring path, not to
seeding.

Worth noting that `TP_MAX_QUERY_LIMIT` is already the effective
ceiling on what a BM25 index scan can return, since backoff stops
there. A `LIMIT` above it is either served by a single deep
scoring pass or not served at all.
