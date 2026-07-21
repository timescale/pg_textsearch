-- Faceted MS-MARCO benchmark - Query workload (pg_textsearch)
--
-- Measures faceted top-k latency:
--
--     SELECT passage_id FROM msmarco_facet
--     WHERE facet_id < <N>                       -- scalar facet, selectivity N%
--     ORDER BY passage_text <@> to_bm25query(q)  -- BM25 ranking
--     LIMIT 10;
--
-- across three modes, for a sweep of facet selectivities:
--
--   on_forced  : enable_facet_pushdown=on,  selectivity gate wide open (1.0)
--                -> pushdown always engages. Isolates the raw effect.
--   on_default : enable_facet_pushdown=on,  default gate (0.12)
--                -> engages only for selective facets (real-world behavior).
--   off        : enable_facet_pushdown=off
--                -> pre-PR path: BM25 top-k ignores the facet, the executor's
--                   Filter discards non-matches and re-drives the scan with an
--                   exponentially growing internal limit until 10 matches
--                   survive. This is the cost the pushdown removes.
--
-- Results with the pushdown on/off are identical (PostgreSQL's Filter node
-- above the index scan is the exact recheck); a parity check asserts this.
--
-- Usage:
--   psql -v nq=200 -f queries.sql

\set ON_ERROR_STOP on
\timing off

\if :{?nq}
\else
  \set nq 200
\endif

-- Force the BM25 index to satisfy the ORDER BY so the facet is a Filter above
-- the index scan (the shape the pushdown targets), independent of row counts.
-- enable_bitmapscan is disabled too: with a btree on the facet column the
-- planner could otherwise switch to a bitmap heap scan + sort and bypass the
-- BM25 pushdown entirely, so this keeps every mode on the same plan shape.
SET enable_seqscan = false;
SET enable_bitmapscan = false;
SET statement_timeout = '10min';

-- Expose the sample size to the DO blocks below (psql does not interpolate
-- :vars inside dollar-quoted blocks).
SET facet.nq = :'nq';

\echo '=== Faceted MS-MARCO Query Benchmark (pg_textsearch) ==='
\echo 'Queries per cell:' :nq
\echo ''

-- Stratified benchmark queries (token buckets 1..8), shared with the
-- non-faceted MS-MARCO benchmark.
DROP TABLE IF EXISTS facet_bench_queries;
CREATE TABLE facet_bench_queries (
    query_id INTEGER,
    query_text TEXT,
    token_bucket INTEGER
);
\copy facet_bench_queries FROM 'benchmarks/datasets/msmarco/benchmark_queries.tsv' WITH (FORMAT text, DELIMITER E'\t')
SELECT 'Loaded ' || COUNT(*) || ' benchmark queries' AS status FROM facet_bench_queries;

-- ------------------------------------------------------------------
-- Core measurement function.
-- Runs the sampled queries for one (mode, facet_n) cell and returns
-- per-query latency percentiles plus the total number of rows produced.
-- ------------------------------------------------------------------
CREATE OR REPLACE FUNCTION facet_bench(
        p_facet_n int,
        p_mode    text,
        p_nq      int)
RETURNS TABLE(p50_ms numeric, p95_ms numeric, p99_ms numeric,
              avg_ms numeric, num_queries int, total_hits bigint)
AS $$
DECLARE
    q record;
    start_ts timestamp;
    end_ts   timestamp;
    times numeric[] := ARRAY[]::numeric[];
    sorted numeric[];
    n int;
    hits bigint;
    hits_sum bigint := 0;
    sql text;
BEGIN
    -- Never let the script-level statement_timeout abort a slow-but-healthy
    -- cell partway through and discard its timings.
    PERFORM set_config('statement_timeout', '0', true);

    -- Configure the pushdown mode for this cell (transaction-local).
    IF p_mode = 'off' THEN
        PERFORM set_config('pg_textsearch.enable_facet_pushdown', 'off', true);
    ELSIF p_mode = 'on_forced' THEN
        PERFORM set_config('pg_textsearch.enable_facet_pushdown', 'on', true);
        PERFORM set_config('pg_textsearch.facet_selectivity_threshold', '1.0', true);
    ELSE  -- on_default
        PERFORM set_config('pg_textsearch.enable_facet_pushdown', 'on', true);
        PERFORM set_config('pg_textsearch.facet_selectivity_threshold', '0.12', true);
    END IF;

    sql := format(
        'SELECT count(*) FROM ('
        '  SELECT passage_id FROM msmarco_facet'
        '  WHERE facet_id < %s'
        '  ORDER BY passage_text <@> to_bm25query($1, ''msmarco_facet_bm25_idx'')'
        '  LIMIT 10) t',
        p_facet_n);

    FOR q IN
        SELECT query_text FROM facet_bench_queries
        ORDER BY query_id LIMIT p_nq
    LOOP
        start_ts := clock_timestamp();
        EXECUTE sql INTO hits USING q.query_text;
        end_ts := clock_timestamp();
        times := array_append(times,
                              EXTRACT(EPOCH FROM (end_ts - start_ts)) * 1000);
        hits_sum := hits_sum + hits;
    END LOOP;

    n := array_length(times, 1);
    SELECT array_agg(t ORDER BY t) INTO sorted FROM unnest(times) t;
    p50_ms := sorted[(n + 1) / 2];
    p95_ms := sorted[(n * 95 + 99) / 100];
    p99_ms := sorted[(n * 99 + 99) / 100];
    avg_ms := (SELECT avg(t) FROM unnest(times) t);
    num_queries := n;
    total_hits := hits_sum;
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- ------------------------------------------------------------------
-- Warm up the cache (index + heap) so the sweep measures steady state.
-- ------------------------------------------------------------------
\echo 'Warming up...'
SELECT * FROM facet_bench(50, 'on_forced', LEAST(:nq, 100));

-- ------------------------------------------------------------------
-- Selectivity sweep. One row per (mode, selectivity) cell.
-- ------------------------------------------------------------------
DROP TABLE IF EXISTS facet_sweep;
CREATE TEMP TABLE facet_sweep(
    mode text, sel_pct int,
    p50_ms numeric, p95_ms numeric, p99_ms numeric, avg_ms numeric,
    num_queries int, total_hits bigint);

\echo ''
\echo '=== Selectivity sweep (facet_id < N) ==='

-- Selectivities: 1%, 5%, 10%, 25%, 50%
DO $$
DECLARE
    sels int[] := ARRAY[1, 5, 10, 25, 50];
    modes text[] := ARRAY['off', 'on_default', 'on_forced'];
    s int;
    m text;
    r record;
    v_nq int := current_setting('facet.nq')::int;
BEGIN
    FOREACH s IN ARRAY sels LOOP
        FOREACH m IN ARRAY modes LOOP
            SELECT * INTO r FROM facet_bench(s, m, v_nq);
            INSERT INTO facet_sweep VALUES
                (m, s, r.p50_ms, r.p95_ms, r.p99_ms, r.avg_ms,
                 r.num_queries, r.total_hits);
            RAISE NOTICE 'FACET_RESULT mode=% sel=% n=% p50=%ms p95=%ms p99=%ms avg=%ms hits=%',
                rpad(m, 10), lpad(s::text, 2) || '%', r.num_queries,
                round(r.p50_ms, 2), round(r.p95_ms, 2), round(r.p99_ms, 2),
                round(r.avg_ms, 2), r.total_hits;
        END LOOP;
    END LOOP;
END;
$$;

-- ------------------------------------------------------------------
-- Summary table + speedup of pushdown vs the off (pre-PR) path.
-- ------------------------------------------------------------------
\echo ''
\echo '=== Summary: avg ms/query by mode and selectivity ==='
SELECT sel_pct AS "sel%",
       round(MAX(avg_ms) FILTER (WHERE mode='off'), 2)        AS off_ms,
       round(MAX(avg_ms) FILTER (WHERE mode='on_default'), 2) AS on_default_ms,
       round(MAX(avg_ms) FILTER (WHERE mode='on_forced'), 2)  AS on_forced_ms,
       round(MAX(avg_ms) FILTER (WHERE mode='off')
             / NULLIF(MAX(avg_ms) FILTER (WHERE mode='on_forced'), 0), 2)
           AS speedup_x
FROM facet_sweep
GROUP BY sel_pct
ORDER BY sel_pct;

\echo ''
\echo '=== Machine-readable results ==='
SELECT 'FACET_SWEEP_ROW '
    || 'mode=' || mode
    || ' sel=' || sel_pct
    || ' p50=' || round(p50_ms, 3)
    || ' p95=' || round(p95_ms, 3)
    || ' p99=' || round(p99_ms, 3)
    || ' avg=' || round(avg_ms, 3)
    || ' n=' || num_queries
    || ' hits=' || total_hits AS row
FROM facet_sweep
ORDER BY sel_pct, mode;

-- ------------------------------------------------------------------
-- Correctness: pushdown on vs off must return identical top-k rows.
-- ------------------------------------------------------------------
\echo ''
\echo '=== Parity check: pushdown on vs off (must be identical) ==='
DO $$
DECLARE
    q record;
    on_ids text[];
    off_ids text[];
    mism int := 0;
    checked int := 0;
    sql_t text;
BEGIN
    sql_t :=
        'SELECT array_agg(passage_id ORDER BY passage_id) FROM ('
        '  SELECT passage_id FROM msmarco_facet WHERE facet_id < 5'
        '  ORDER BY passage_text <@> to_bm25query($1, ''msmarco_facet_bm25_idx''), passage_id'
        '  LIMIT 10) t';

    FOR q IN SELECT query_text FROM facet_bench_queries
             ORDER BY query_id LIMIT 50 LOOP
        PERFORM set_config('pg_textsearch.enable_facet_pushdown', 'on', true);
        PERFORM set_config('pg_textsearch.facet_selectivity_threshold', '1.0', true);
        EXECUTE sql_t INTO on_ids USING q.query_text;

        PERFORM set_config('pg_textsearch.enable_facet_pushdown', 'off', true);
        EXECUTE sql_t INTO off_ids USING q.query_text;

        checked := checked + 1;
        IF on_ids IS DISTINCT FROM off_ids THEN
            mism := mism + 1;
            RAISE WARNING 'PARITY MISMATCH for query: %', q.query_text;
        END IF;
    END LOOP;

    RAISE NOTICE 'PARITY_CHECK checked=% mismatches=%', checked, mism;
    IF mism > 0 THEN
        RAISE EXCEPTION 'Facet pushdown changed results in % of % queries',
            mism, checked;
    END IF;
END;
$$;

DROP FUNCTION facet_bench(int, text, int);
DROP TABLE facet_bench_queries;

\echo ''
\echo '=== Faceted MS-MARCO Query Benchmark Complete (pg_textsearch) ==='
