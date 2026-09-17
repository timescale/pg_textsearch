# Issue #435: Bind the filtered top-K seed per scan

Issue [#435](https://github.com/timescale/pg_textsearch/issues/435).
Implementation: the existing `src/index/limit.c` module. No separate
seed source or header is needed.

## Problem

The old planner-to-executor handoff used one backend-local slot keyed by
index OID, consumed on first read. Two scans of the same index with
different filters overwrite each other's seed. The first scan receives
the last-planned seed and later scans fall back to `default_limit`.
Planning also need not run when a cached plan is executed, and costing
can visit paths that are never chosen.

Each scan should start at its own estimated depth:

```
k = LIMIT count + OFFSET
seed = ceil(filtered_seed_margin * k / filter_selectivity)
```

The existing helper bounds the selectivity-based expansion by
`TP_MAX_QUERY_LIMIT` and never reduces the user's k. With seeding off,
no filter, or an unusable estimate, it returns k unchanged. This is an
initial batch size, not a replacement for the executor's Filter or
Limit. Existing backoff handles estimates that are too small.

## Scope: direct Limit -> BM25 IndexScan pairs

The planner walks the finished plan and attaches a seed hint only when a
Limit's immediate child is a BM25 IndexScan. There is no inherited Limit
in the planner walk.

```
Append
  Limit 10
    IndexScan on idx, rare filter     -> its own seed
  Limit 10
    IndexScan on idx, common filter   -> its own seed
```

The planner walks ordinary children, Append/MergeAppend arms, SubqueryScan
and CustomScan children, and the separate PlannedStmt subplan list. This
finds local pairs inside UNION ALL arms, materialized CTEs, and correlated
subqueries. An outer Limit cannot leak into a subplan because matching is
local.

An intervening node means no binding, even when it preserves ordering.
For example, `Limit -> LockRows -> IndexScan` and
`Limit -> MergeAppend -> IndexScans` use the default and backoff. A
local pair below such a node can still be seeded independently.
Supporting additional shapes is an optional performance enhancement;
it is not needed to address the motivating UNION ALL case.

The hint is copied into each plan-local bm25query value, so identical
queries on one index still carry independent seeds and cached plans retain
their hints. Runtime query or LIMIT expressions remain unhinted and use
backoff.

## Handoff to the access method

The planner copies the `bm25query` value in the IndexScan's ORDER BY
expression and appends an internal trailer containing raw `k` and filter
selectivity. `tp_rescan` reads that value from its ORDER BY ScanKey and
computes the seed with the current GUC values.

The handoff is carried by the query value itself:

```
Planner
  find Limit -> IndexScan
  copy the scan's bm25query and append k/selectivity

IndexNext
  index_beginscan -> tp_beginscan
  index_rescan(..., iss_OrderByKeys, ...)
    tp_rescan -> read hint -> so->limit = seed
```

No executor hook, global registry, ScanKey identity, or reset callback is
needed. A missing hint uses `tp_default_limit` plus existing backoff.

## Computing the seed

Only constant LIMIT/OFFSET and constant bm25query values are annotated.
Runtime parameters, InitPlans, and arbitrary expressions are left
unhinted and use normal backoff. Cached constant plans recompute the
final seed from the current seeding GUCs at scan time.

Include OFFSET in k. A nonpositive count or a sum at or above INT_MAX
means no binding. Do not clamp to INT_MAX: the scan uses its internal
limit to size an allocation, so that would request multiple gigabytes.
LIMIT ALL and unsupported Limit expressions also fall back.

Require one ORDER BY key and no index quals. Residual executor Filter
quals are expected. For a filtered scan with positive reltuples,
estimate selectivity as `plan_rows / heap_relation.reltuples`. This is
an approximation from the plan's row estimate and relation statistics,
not a new call to planner selectivity functions. A missing or unusable
estimate leaves the seed at k.

## Validation

`test/sql/filtered_seed.sql` checks result parity and counts scoring
passes through `bm25_debug_scoring_passes()`. Results alone cannot show
whether a seed reached the correct scan.

Coverage includes:

- Per-arm seeds for UNION ALL scans of the same index.
- Different raw LIMITs with seeding off as well as on.
- Correlated SubPlan rescans restoring their seed each time.
- Materialized CTE and scalar InitPlan scans reached by core's walker.
- Generic prepared plans with supported and unsupported LIMIT params.
- OFFSET and the INT_MAX saturation boundary.
- An explicit Limit -> LockRows -> IndexScan plan using fallback,
  with both scoring-pass and result-parity checks.

`benchmarks/sql/filtered_seed.sql` compares single scans and multi-arm
queries with uniform and mixed selectivities. Compare the seed-on
columns across implementations: the old shared-slot version and the
per-scan versions assign raw k differently even with seeding off.

### Local comparison with the original PR

PostgreSQL 18.4, 200,000 documents, median of seven measured executions
per cell, using the existing benchmark on the same dedicated server
before and after installing the simplified binary:

| Shape | Selectivity | LIMIT | PR (ms) | Simplified (ms) | Passes |
| --- | --- | --- | --- | --- | --- |
| single | 0.001 | 10 | 9.30 | 9.61 | 1 |
| single | 0.001 | 100 | 27.70 | 28.75 | 1 |
| single | 0.01 | 10 | 2.38 | 2.32 | 1 |
| single | 0.1 | 10 | 1.69 | 1.58 | 1 |
| union2 | 0.001 | 10 | 18.80 | 18.08 | 2 |
| union3 | 0.001 | 10 | 28.80 | 27.14 | 3 |
| union3 | mixed | 10 | 61.40 | 58.05 | 5 |

The pass counts are unchanged in every cell. These sequential local
runs establish that the tested shapes retain their seeding behavior;
the timing differences are not evidence of a new speedup. Shapes with
an intervening node intentionally lose seeding and may take more work.

## Separate follow-ups

Large explicit LIMITs below INT_MAX still size potentially excessive
allocations through `so->limit`. Merely capping them is not a fix:
backoff itself stops at `TP_MAX_QUERY_LIMIT`, so a smaller initial
batch can truncate a larger requested result. Fixing allocation growth
and the existing scan ceiling requires a separate scoring-path change.

A separate, previously identified PG18 case also needs investigation:
with a partial BM25 index and `enable_seqscan = off`, a scan without an
ORDER BY may be selected despite its infinite cost. The no-query-text
path may yield zero rows. This has not been reproduced here and is not
changed by the seed simplification.
