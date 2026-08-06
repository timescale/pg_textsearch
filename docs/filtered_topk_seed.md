# Selectivity-seeded top-K for filtered BM25 search

Status: design (2026-08-06)
Supersedes: the faceted-search *allow-list pushdown* direction (PR #408 and the
`tjgreen42/facet-bloom-allowlist` exploration). See
`docs/superpowers` session notes and `benchmarks/facet` for the data behind
this decision.

## Problem

A filtered BM25 top-k query —

```sql
SELECT ... FROM docs
WHERE category = 'news'                       -- a scalar filter ("facet")
ORDER BY body <@> to_bm25query('...', 'idx')  -- BM25 ranking
LIMIT 10;
```

— is planned as a BM25 index scan (top-k by score) with the filter as a
**Filter node above the scan** and a Limit on top. The BM25 scan first
produces its internal top-k (k rows by score); the Filter discards
non-matching rows; if fewer than k survive, the scan **re-drives with an
exponentially growing internal limit** (k, 2k, 4k, …) until k matching rows
survive. This backoff **re-scores from scratch each round**.

For a filter of selectivity `s`, surfacing k matches needs ~`k/s` scored rows,
so the backoff performs ~`log2(1/s)` rounds (≈10 at 0.1% selectivity), each
re-reading the query terms' posting lists. Measured on MS-MARCO v2 (138M rows),
this backoff waste makes filtered top-k **~5× slower than necessary** at low
selectivity (e.g. ~1074 ms vs ~202 ms average at 0.1%).

## Approach: seed the internal top-K from estimated selectivity

Instead of starting the internal top-K at k and backing off, **seed it up
front** to about the depth a single pass needs:

```
seed_K = ceil(margin · k / s_est)     (capped at TP_MAX_QUERY_LIMIT)
```

where `s_est` is the planner's estimate of the filter's selectivity and
`margin` (default 3) absorbs the variance around the expected rank `k/s` so the
true top-k matching rows are captured in one scoring pass with high
probability. The **existing backoff remains as the correctness safety net**:
if the estimate under-shot, the scan still re-drives until k matches survive.

WAND/Block-Max WAND runs at full efficiency (nothing constrains it), and the
executor's Filter applies the predicate to the ~`seed_K` emitted rows. Results
are **identical** to the un-seeded plan — the seed only changes how deep the
single scoring pass goes, not which rows win.

### Why not push the filter into scoring (an allow-list)?

We prototyped and benchmarked the allow-list *pushdown* (build a set of
matching row ids, consult it during BMW scoring) with four membership
structures — a sorted-TID array, a Bloom filter, an open-addressing hash set,
and a radix-tree `TidStore` — plus a per-segment doc_id bitset. All lost to
seeded-K (exploration-phase averages; the baseline and seeded-K columns were
later reconfirmed by a clean same-binary `filtered_seed` off/on A/B — see
below):

| selectivity | baseline (backoff) | best pushdown (TidStore) | seeded-K |
|---|---:|---:|---:|
| 0.1% | 1074 ms | 417 ms | **202 ms** |
| 1%   | 621 ms  | 289 ms | **109 ms** |
| 5%   | 402 ms  | 427 ms | **86 ms**  |

The production A/B (same binary, only `pg_textsearch.filtered_seed` flipped,
50 warm queries, margin 3) on the 138M-row corpus gives seed_off → seed_on
averages of 1074→202 ms (0.1%), 621→109 ms (1%), 402→86 ms (5%) — a 4.7–5.7×
speedup with no censored queries and no crossover.

Profiling showed the pushdown's cost is dominated (~65%) by translating each
scoring candidate's segment `doc_id` into a heap `ctid` to consult the
ctid-keyed allow-list (`tp_segment_lookup_ctid`) — a cost that exists because
pg_textsearch's facet lives on heap ctids while scoring runs in `doc_id` space,
with no cheap scan-time reverse map. Lucene/Tantivy achieve fast filtered
search with the same "bitmap-as-filter" idea only because their filter is
**doc-id-native** (same id space as the scorer) and **cached** across queries —
capabilities pg_textsearch's storage lacks. Even a *perfect-world* pushdown
(doc-id resolution and the allow-list build made free/amortized) beats seeded-K
by only ~6–10% at 1–5% and 1.65× at 0.1% — an unreachable ceiling that would
require a new doc-id-native facet store. Seeded-K captures nearly all of that
for a ~15-line change and no new storage.

## Design

**Planner** (`src/planner/cost.c`, `tp_costestimate`): the BM25 LIMIT pushdown
already stores the query's `LIMIT k` for the scan (`tp_store_query_limit`). Add:
when `filtered_seed` is on and the scanned relation has restriction clauses
(`rel->baserestrictinfo != NIL`, i.e. a Filter will sit above the BM25 index
scan), estimate their combined selectivity

```c
s = clauselist_selectivity(root, rel->baserestrictinfo, rel->relid,
                           JOIN_INNER, NULL);
```

and, if `0 < s < 1`, store `seed_K = min(TP_MAX_QUERY_LIMIT,
ceil(margin · k / s))` instead of `k`. With no restriction clauses (no Filter),
store `k` unchanged — non-filtered top-k is untouched.

**Execution**: unchanged. The scan uses the stored (seeded) limit as its
internal top-K (`so->limit`); the existing backoff re-drive (`src/access/scan.c`)
refills if the estimate under-shot; the executor Filter applies the predicate;
the Limit node returns k. No allow-list, no membership test, no new per-scan
structure, no `bmw.c` changes.

**Correctness**: the seed is a pure performance hint. The Filter + backoff
guarantee the exact top-k matching rows regardless of the seed value, so
results are identical to the un-seeded plan (and to a plain sequential scan).

## GUCs

| GUC | Type | Default | Description |
|-----|------|---------|-------------|
| `pg_textsearch.filtered_seed` | bool | on | Seed the BM25 internal top-K from estimated filter selectivity to avoid backoff re-drives on filtered top-k queries. |
| `pg_textsearch.filtered_seed_margin` | double | 3.0 | `seed_K = ceil(margin · k / selectivity)`. Higher margin captures the true top-k matching rows in one pass more often, at the cost of scoring deeper. Range [1, 1000]. |

## Testing

- **Parity**: for filtered top-k queries across selectivities and several query
  terms, the result set with `filtered_seed` on must equal the result set with
  it off (identical top-k). 0 mismatches required.
- **Estimate-robustness**: a query whose filter the planner badly
  mis-estimates must still return correct results (backoff safety net) — assert
  correctness, not latency.
- **Regression**: full SQL suite on PostgreSQL 17 and 18, unchanged.
- **No-filter no-op**: a top-k query with no WHERE clause stores the raw k
  (seed inactive) — verify plan/behavior unchanged.

## Benchmark

On the live MS-MARCO v2 instance: filtered top-10 across 0.1 / 1 / 5%
selectivity, `filtered_seed` on vs off, confirming ~5× at low selectivity and
no pushdown/baseline crossover (seeded-K wins throughout).

## Out of scope / future

- **Doc-id-native facet storage** (a DocValues-style columnar facet column, or
  a cached per-segment doc_id bitset) would enable a genuine Lucene-style
  pushdown. It is a larger architectural project and only worth revisiting if
  seeded-K's executor-Filter heap-fetches (its one cost the pushdown ceiling
  avoided, and only materially at extreme selectivity) ever dominate.
- **Correlated facets**: `k/s` assumes the filter is uncorrelated with BM25
  score. A strongly anti-correlated filter inflates the needed depth; the
  backoff still guarantees correctness, and the margin can be raised. Worth a
  spot-check but not a blocker.
