-- Faceted MS-MARCO benchmark - Query workload (ParadeDB / pg_search)
--
-- Counterpart of ../pg_textsearch/queries.sql. Measures faceted top-k latency
-- using ParadeDB's native faceted search: a boolean query that combines the
-- BM25 text match with a numeric range on the indexed facet field, so the
-- filter is applied inside the index (the equivalent of pg_textsearch's facet
-- pushdown). Reported across the same facet-selectivity sweep.
--
--     SELECT passage_id FROM msmarco_facet_pdb
--     WHERE passage_id @@@ paradedb.boolean(must => ARRAY[
--             paradedb.match('passage_text', q),
--             paradedb.range('facet_id', int4range(NULL, N, '[)'))])
--     ORDER BY paradedb.score(passage_id) DESC
--     LIMIT 10;
--
-- Usage:
--   psql -v nq=200 -f queries.sql

\set ON_ERROR_STOP on
\timing off

\if :{?nq}
\else
  \set nq 200
\endif

SET statement_timeout = '10min';
SET facet.nq = :'nq';

\echo '=== Faceted MS-MARCO Query Benchmark (ParadeDB) ==='
\echo 'Queries per cell:' :nq
\echo ''

DROP TABLE IF EXISTS facet_bench_queries;
CREATE TABLE facet_bench_queries (
    query_id INTEGER,
    query_text TEXT,
    token_bucket INTEGER
);
\copy facet_bench_queries FROM 'benchmarks/datasets/msmarco/benchmark_queries.tsv' WITH (FORMAT text, DELIMITER E'\t')
SELECT 'Loaded ' || COUNT(*) || ' benchmark queries' AS status FROM facet_bench_queries;

CREATE OR REPLACE FUNCTION facet_bench_pdb(
        p_facet_n int,
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
    PERFORM set_config('statement_timeout', '0', true);

    sql := format(
        'SELECT count(*) FROM ('
        '  SELECT passage_id FROM msmarco_facet_pdb'
        '  WHERE passage_id @@@ paradedb.boolean(must => ARRAY['
        '          paradedb.match(''passage_text'', $1),'
        '          paradedb.range(''facet_id'', int4range(NULL, %s, ''[)''))])'
        '  ORDER BY paradedb.score(passage_id) DESC'
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

\echo 'Warming up...'
SELECT * FROM facet_bench_pdb(50, LEAST(:nq, 100));

DROP TABLE IF EXISTS facet_sweep_pdb;
CREATE TEMP TABLE facet_sweep_pdb(
    sel_pct int,
    p50_ms numeric, p95_ms numeric, p99_ms numeric, avg_ms numeric,
    num_queries int, total_hits bigint);

\echo ''
\echo '=== Selectivity sweep (facet_id < N) ==='
DO $$
DECLARE
    sels int[] := ARRAY[1, 5, 10, 25, 50];
    s int;
    r record;
    v_nq int := current_setting('facet.nq')::int;
BEGIN
    FOREACH s IN ARRAY sels LOOP
        SELECT * INTO r FROM facet_bench_pdb(s, v_nq);
        INSERT INTO facet_sweep_pdb VALUES
            (s, r.p50_ms, r.p95_ms, r.p99_ms, r.avg_ms,
             r.num_queries, r.total_hits);
        RAISE NOTICE 'FACET_RESULT mode=paradedb   sel=% n=% p50=%ms p95=%ms p99=%ms avg=%ms hits=%',
            lpad(s::text, 2) || '%', r.num_queries,
            round(r.p50_ms, 2), round(r.p95_ms, 2), round(r.p99_ms, 2),
            round(r.avg_ms, 2), r.total_hits;
    END LOOP;
END;
$$;

\echo ''
\echo '=== Summary: avg ms/query by selectivity (ParadeDB) ==='
SELECT sel_pct AS "sel%",
       round(avg_ms, 2) AS paradedb_ms,
       round(p50_ms, 2) AS p50_ms,
       round(p99_ms, 2) AS p99_ms
FROM facet_sweep_pdb
ORDER BY sel_pct;

\echo ''
\echo '=== Machine-readable results ==='
SELECT 'FACET_SWEEP_ROW '
    || 'mode=paradedb'
    || ' sel=' || sel_pct
    || ' p50=' || round(p50_ms, 3)
    || ' p95=' || round(p95_ms, 3)
    || ' p99=' || round(p99_ms, 3)
    || ' avg=' || round(avg_ms, 3)
    || ' n=' || num_queries
    || ' hits=' || total_hits AS row
FROM facet_sweep_pdb
ORDER BY sel_pct;

DROP FUNCTION facet_bench_pdb(int, int);
DROP TABLE facet_bench_queries;

\echo ''
\echo '=== Faceted MS-MARCO Query Benchmark Complete (ParadeDB) ==='
