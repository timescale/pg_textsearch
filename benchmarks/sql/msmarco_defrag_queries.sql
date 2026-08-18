-- MS-MARCO v2 defrag benchmark: query latency by token bucket.
--
-- Expects:
--   - msmarco_v2_bench with BM25 index msmarco_v2_bm25_idx
--   - benchmark_queries (query_id int, query_text text, token_bucket int)
--     already loaded
--   - :run_label psql variable set to 'scattered' or 'contiguous'
--
-- Writes results into defrag_bench_results (created if absent).

\set ON_ERROR_STOP on

CREATE TABLE IF NOT EXISTS defrag_bench_results (
    run_label  TEXT,
    bucket     INT,
    p50_ms     NUMERIC,
    p95_ms     NUMERIC,
    p99_ms     NUMERIC,
    avg_ms     NUMERIC,
    num_queries INT,
    total_results BIGINT
);

-- Benchmark one token bucket: run all queries, collect per-query
-- wall-clock times, return percentiles.
CREATE OR REPLACE FUNCTION _defrag_bench_bucket(bucket_id int)
RETURNS TABLE(p50_ms numeric, p95_ms numeric, p99_ms numeric,
              avg_ms numeric, num_queries int, total_results bigint)
AS $$
DECLARE
    q           record;
    start_ts    timestamp;
    end_ts      timestamp;
    times       numeric[];
    sorted_times numeric[];
    n           int;
    result_count bigint;
    results_sum  bigint := 0;
BEGIN
    times := ARRAY[]::numeric[];

    FOR q IN SELECT query_text
             FROM benchmark_queries
             WHERE token_bucket = bucket_id
             ORDER BY query_id
    LOOP
        start_ts := clock_timestamp();
        EXECUTE
            'SELECT COUNT(*) FROM ('
            '  SELECT passage_id FROM msmarco_v2_bench'
            '  ORDER BY passage_text <@>'
            '    to_bm25query($1, ''msmarco_v2_bm25_idx'')'
            '  LIMIT 10'
            ') t'
            INTO result_count USING q.query_text;
        end_ts := clock_timestamp();
        times := array_append(
            times,
            EXTRACT(EPOCH FROM (end_ts - start_ts)) * 1000);
        results_sum := results_sum + result_count;
    END LOOP;

    n := array_length(times, 1);
    IF n IS NULL OR n = 0 THEN
        RETURN;
    END IF;

    SELECT array_agg(t ORDER BY t)
      INTO sorted_times
      FROM unnest(times) t;

    p50_ms  := sorted_times[(n + 1) / 2];
    p95_ms  := sorted_times[(n * 95 + 99) / 100];
    p99_ms  := sorted_times[(n * 99 + 99) / 100];
    avg_ms  := (SELECT AVG(t) FROM unnest(times) t);
    num_queries   := n;
    total_results := results_sum;
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- Warmup: run ALL benchmark queries once to fully prime caches
\echo 'Warming up (all 691 queries)...'
DO $$
DECLARE q record;
BEGIN
    FOR q IN SELECT query_text
             FROM benchmark_queries
             ORDER BY query_id
    LOOP
        EXECUTE
            'SELECT passage_id FROM msmarco_v2_bench'
            ' ORDER BY passage_text <@>'
            '   to_bm25query($1, ''msmarco_v2_bm25_idx'')'
            ' LIMIT 10'
            USING q.query_text;
    END LOOP;
END;
$$;

-- Run each bucket
\echo 'Running bucket 1 (1 token)...'
INSERT INTO defrag_bench_results
    SELECT :'run_label', 1, * FROM _defrag_bench_bucket(1);

\echo 'Running bucket 2 (2 tokens)...'
INSERT INTO defrag_bench_results
    SELECT :'run_label', 2, * FROM _defrag_bench_bucket(2);

\echo 'Running bucket 3 (3 tokens)...'
INSERT INTO defrag_bench_results
    SELECT :'run_label', 3, * FROM _defrag_bench_bucket(3);

\echo 'Running bucket 4 (4 tokens)...'
INSERT INTO defrag_bench_results
    SELECT :'run_label', 4, * FROM _defrag_bench_bucket(4);

\echo 'Running bucket 5 (5 tokens)...'
INSERT INTO defrag_bench_results
    SELECT :'run_label', 5, * FROM _defrag_bench_bucket(5);

\echo 'Running bucket 6 (6 tokens)...'
INSERT INTO defrag_bench_results
    SELECT :'run_label', 6, * FROM _defrag_bench_bucket(6);

\echo 'Running bucket 7 (7 tokens)...'
INSERT INTO defrag_bench_results
    SELECT :'run_label', 7, * FROM _defrag_bench_bucket(7);

\echo 'Running bucket 8 (8+ tokens)...'
INSERT INTO defrag_bench_results
    SELECT :'run_label', 8, * FROM _defrag_bench_bucket(8);

DROP FUNCTION _defrag_bench_bucket;

-- Show results for this run
SELECT bucket,
       round(p50_ms, 2) AS p50,
       round(p95_ms, 2) AS p95,
       round(p99_ms, 2) AS p99,
       round(avg_ms, 2) AS avg,
       num_queries AS n
FROM defrag_bench_results
WHERE run_label = :'run_label'
ORDER BY bucket;
