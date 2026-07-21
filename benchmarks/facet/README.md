# Faceted BM25 benchmark

Measures the pg_textsearch faceted-search filter pushdown
(`pg_textsearch.enable_facet_pushdown`) on MS-MARCO, and optionally compares
against ParadeDB `pg_search` native faceted search on identical data and
queries.

The query shape is `WHERE facet_id < N ORDER BY passage_text <@> q LIMIT 10` —
a scalar facet paired with a BM25 top-k. A synthetic uniform `facet_id` column
(derived from a portable md5 of the passage id, so both systems see identical
assignments) sweeps facet selectivity.

## Layout

- `pg_textsearch/load.sql`, `pg_textsearch/queries.sql` — load + query sweep for
  pg_textsearch.
- `paradedb/load.sql`, `paradedb/queries.sql` — the same for ParadeDB
  `pg_search`.
- `run_facet_benchmark.sh` — driver for one or both systems.

## Prerequisites

MS-MARCO `collection.tsv` under `benchmarks/datasets/msmarco/data/` (see
`benchmarks/datasets/msmarco/download.sh`). For the ParadeDB comparison, a
running ParadeDB instance with `pg_search`.

## Usage

```sh
# pg_textsearch only: load, then sweep
PG_PSQL="psql -h /tmp -p 5408 -U postgres -d postgres" \
  ./run_facet_benchmark.sh --systems pg --load --nq 100
```

`pg_textsearch/load.sql` reads two optional environment overrides:

- `MAXROWS` — cap the number of leading passages loaded (CI-scale vs full).
- `FACET_BUCKETS` — number of uniform facet buckets (default 100 =
  1%-granularity; larger values give finer sub-1% selectivities).

## Results (1M passages, PostgreSQL 17)

Average ms/query, pushdown (index-backed allow-list) vs post-filter baseline,
with the BM25-scan plan forced (`enable_bitmapscan=off`) to isolate the
pushdown:

| facet selectivity | baseline | pushdown |
|------------------:|---------:|---------:|
| 0.2% | 27.7 | 12.5 |
| 0.5% | 16.9 | 10.5 |
| 1% | 12.3 | 8.9 |
| 2% | 8.9 | 9.0 |
| 3% | 7.4 | 9.5 |
| 5% | 6.2 | 11.5 |

The pushdown cost scales with the number of matching rows, so it wins on
selective facets and loses on broad ones. The crossover is ~1.9% at 1M and ~3%
at 250k; the `facet_selectivity_threshold` default (0.02) sits at the 1M
crossover. The heap-scan fallback runs the same workload at 114–206 ms across
1–50% selectivity; the index scan brings it to 7–80 ms.
