-- MS MARCO Parallel Build Scaling Benchmark
-- Tests index build performance with varying numbers of parallel workers
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f parallel_scaling.sql
--
-- Outputs:
--   PARALLEL_BUILD_RESULT: workers=N, build_time_ms=X, query_avg_ms=Y
--
-- Assumes data is already loaded (run load_data_only.sql first)

\set ON_ERROR_STOP on
\timing on

\echo '=== Parallel Build Scaling Benchmark ==='
\echo 'Testing index build performance with 0, 1, 2, 4 workers'
\echo ''

-- Verify data is loaded
SELECT COUNT(*) as passage_count FROM msmarco_passages;
\gset

\if :passage_count < 100000
    \echo 'ERROR: MS MARCO data not loaded or too small. Need at least 100K passages.'
    \echo 'Run load_data_only.sql or load.sql first.'
    \quit
\endif

\echo 'Found ' :passage_count ' passages'
\echo ''

-- Sample queries for quick performance test after each build
CREATE TEMP TABLE bench_queries (query_text TEXT);
INSERT INTO bench_queries VALUES
    ('what is machine learning'),
    ('how to bake a cake'),
    ('python programming tutorial'),
    ('best restaurants in new york'),
    ('climate change effects'),
    ('history of the internet'),
    ('how does gps work'),
    ('benefits of exercise'),
    ('quantum computing explained'),
    ('renewable energy sources');

-- Function to run quick query benchmark
CREATE OR REPLACE FUNCTION bench_queries(index_name TEXT)
RETURNS TABLE(avg_ms NUMERIC, min_ms NUMERIC, max_ms NUMERIC) AS $$
DECLARE
    start_ts TIMESTAMP;
    end_ts TIMESTAMP;
    total_ms NUMERIC := 0;
    min_time NUMERIC := 999999;
    max_time NUMERIC := 0;
    query_time NUMERIC;
    q TEXT;
    iter INT;
BEGIN
    -- Warm up
    FOR q IN SELECT query_text FROM bench_queries LOOP
        PERFORM ctid FROM msmarco_passages
        ORDER BY passage_text <@> to_bm25query(q, index_name)
        LIMIT 10;
    END LOOP;

    -- Timed runs (3 iterations)
    FOR iter IN 1..3 LOOP
        FOR q IN SELECT query_text FROM bench_queries LOOP
            start_ts := clock_timestamp();
            PERFORM ctid FROM msmarco_passages
            ORDER BY passage_text <@> to_bm25query(q, index_name)
            LIMIT 10;
            end_ts := clock_timestamp();
            query_time := EXTRACT(EPOCH FROM (end_ts - start_ts)) * 1000;
            total_ms := total_ms + query_time;
            min_time := LEAST(min_time, query_time);
            max_time := GREATEST(max_time, query_time);
        END LOOP;
    END LOOP;

    avg_ms := total_ms / 30;  -- 10 queries * 3 iterations
    min_ms := min_time;
    max_ms := max_time;
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

----------------------------------------------------------------------
-- Test with 0 workers (serial build)
----------------------------------------------------------------------
\echo ''
\echo '=== Testing with 0 workers (serial build) ==='

DROP INDEX IF EXISTS msmarco_bm25_idx;
SET max_parallel_maintenance_workers = 0;
SHOW max_parallel_maintenance_workers;

\timing on
CREATE INDEX msmarco_bm25_idx ON msmarco_passages
    USING bm25(passage_text) WITH (text_config='english');
\timing off

-- Capture build time from psql timing
\echo 'PARALLEL_BUILD_WORKERS: 0'

-- Get index info
SELECT bm25_summarize_index('msmarco_bm25_idx');

-- Query benchmark
SELECT 'PARALLEL_QUERY_RESULT: workers=0, avg_ms=' || ROUND(avg_ms, 2) ||
       ', min_ms=' || ROUND(min_ms, 2) || ', max_ms=' || ROUND(max_ms, 2)
FROM bench_queries('msmarco_bm25_idx');

----------------------------------------------------------------------
-- Test with 1 worker
----------------------------------------------------------------------
\echo ''
\echo '=== Testing with 1 worker ==='

DROP INDEX IF EXISTS msmarco_bm25_idx;
SET max_parallel_maintenance_workers = 1;
SHOW max_parallel_maintenance_workers;

\timing on
CREATE INDEX msmarco_bm25_idx ON msmarco_passages
    USING bm25(passage_text) WITH (text_config='english');
\timing off

\echo 'PARALLEL_BUILD_WORKERS: 1'

SELECT bm25_summarize_index('msmarco_bm25_idx');

SELECT 'PARALLEL_QUERY_RESULT: workers=1, avg_ms=' || ROUND(avg_ms, 2) ||
       ', min_ms=' || ROUND(min_ms, 2) || ', max_ms=' || ROUND(max_ms, 2)
FROM bench_queries('msmarco_bm25_idx');

----------------------------------------------------------------------
-- Test with 2 workers
----------------------------------------------------------------------
\echo ''
\echo '=== Testing with 2 workers ==='

DROP INDEX IF EXISTS msmarco_bm25_idx;
SET max_parallel_maintenance_workers = 2;
SHOW max_parallel_maintenance_workers;

\timing on
CREATE INDEX msmarco_bm25_idx ON msmarco_passages
    USING bm25(passage_text) WITH (text_config='english');
\timing off

\echo 'PARALLEL_BUILD_WORKERS: 2'

SELECT bm25_summarize_index('msmarco_bm25_idx');

SELECT 'PARALLEL_QUERY_RESULT: workers=2, avg_ms=' || ROUND(avg_ms, 2) ||
       ', min_ms=' || ROUND(min_ms, 2) || ', max_ms=' || ROUND(max_ms, 2)
FROM bench_queries('msmarco_bm25_idx');

----------------------------------------------------------------------
-- Test with 4 workers
----------------------------------------------------------------------
\echo ''
\echo '=== Testing with 4 workers ==='

DROP INDEX IF EXISTS msmarco_bm25_idx;
SET max_parallel_maintenance_workers = 4;
SHOW max_parallel_maintenance_workers;

\timing on
CREATE INDEX msmarco_bm25_idx ON msmarco_passages
    USING bm25(passage_text) WITH (text_config='english');
\timing off

\echo 'PARALLEL_BUILD_WORKERS: 4'

SELECT bm25_summarize_index('msmarco_bm25_idx');

SELECT 'PARALLEL_QUERY_RESULT: workers=4, avg_ms=' || ROUND(avg_ms, 2) ||
       ', min_ms=' || ROUND(min_ms, 2) || ', max_ms=' || ROUND(max_ms, 2)
FROM bench_queries('msmarco_bm25_idx');

----------------------------------------------------------------------
-- Summary
----------------------------------------------------------------------
\echo ''
\echo '=== Parallel Scaling Benchmark Complete ==='
\echo 'Review PARALLEL_BUILD_WORKERS and PARALLEL_QUERY_RESULT lines above'
\echo 'Build times shown in psql timing output after each CREATE INDEX'

-- Cleanup
DROP FUNCTION IF EXISTS bench_queries(TEXT);
