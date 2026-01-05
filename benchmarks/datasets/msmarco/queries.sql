-- MS MARCO Passage Ranking - Query Benchmarks
-- Runs various query workloads against the indexed MS MARCO collection
-- Outputs structured timing data for historical tracking

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO Query Benchmarks ==='
\echo ''

-- Load benchmark queries (pre-sampled by token count)
\echo 'Loading benchmark queries...'
DROP TABLE IF EXISTS benchmark_queries;
CREATE TABLE benchmark_queries (
    query_id INTEGER,
    query_text TEXT,
    token_bucket INTEGER
);
\copy benchmark_queries FROM 'benchmarks/datasets/msmarco/benchmark_queries.tsv' WITH (FORMAT text, DELIMITER E'\t')

-- Verify load
SELECT 'Loaded ' || COUNT(*) || ' benchmark queries' as status FROM benchmark_queries;
SELECT token_bucket, COUNT(*) as count FROM benchmark_queries GROUP BY token_bucket ORDER BY token_bucket;

-- Warm up: run queries from each bucket to ensure index is cached
\echo ''
\echo 'Warming up index...'
DO $$
DECLARE
    q record;
BEGIN
    FOR q IN SELECT query_text FROM benchmark_queries ORDER BY random() LIMIT 50 LOOP
        EXECUTE 'SELECT passage_id FROM msmarco_passages
                 ORDER BY passage_text <@> to_bm25query($1, ''msmarco_bm25_idx'')
                 LIMIT 10' USING q.query_text;
    END LOOP;
END;
$$;

-- ============================================================
-- Benchmark 1: Query Latency by Token Count
-- ============================================================
\echo ''
\echo '=== Benchmark 1: Query Latency by Token Count ==='
\echo 'Running 100 queries per token bucket, reporting p50/p95/p99'
\echo ''

-- Function to benchmark a bucket and return percentiles
CREATE OR REPLACE FUNCTION benchmark_bucket(bucket int)
RETURNS TABLE(p50_ms numeric, p95_ms numeric, p99_ms numeric, avg_ms numeric, num_queries int) AS $$
DECLARE
    q record;
    start_ts timestamp;
    end_ts timestamp;
    times numeric[];
    sorted_times numeric[];
    n int;
BEGIN
    times := ARRAY[]::numeric[];

    FOR q IN SELECT query_text FROM benchmark_queries WHERE token_bucket = bucket ORDER BY query_id LOOP
        start_ts := clock_timestamp();
        EXECUTE 'SELECT passage_id FROM msmarco_passages
                 ORDER BY passage_text <@> to_bm25query($1, ''msmarco_bm25_idx'')
                 LIMIT 10' USING q.query_text;
        end_ts := clock_timestamp();
        times := array_append(times, EXTRACT(EPOCH FROM (end_ts - start_ts)) * 1000);
    END LOOP;

    n := array_length(times, 1);
    SELECT array_agg(t ORDER BY t) INTO sorted_times FROM unnest(times) t;

    p50_ms := sorted_times[(n + 1) / 2];
    p95_ms := sorted_times[(n * 95 + 99) / 100];
    p99_ms := sorted_times[(n * 99 + 99) / 100];
    avg_ms := (SELECT AVG(t) FROM unnest(times) t);
    num_queries := n;
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- Run benchmarks for each token bucket
\echo 'Token bucket 1 (1 search token):'
SELECT 'LATENCY_BUCKET_1: p50=' || round(p50_ms, 2) || 'ms p95=' || round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' || round(avg_ms, 2) || 'ms (n=' || num_queries || ')' as result
FROM benchmark_bucket(1);

\echo ''
\echo 'Token bucket 2 (2 search tokens):'
SELECT 'LATENCY_BUCKET_2: p50=' || round(p50_ms, 2) || 'ms p95=' || round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' || round(avg_ms, 2) || 'ms (n=' || num_queries || ')' as result
FROM benchmark_bucket(2);

\echo ''
\echo 'Token bucket 3 (3 search tokens):'
SELECT 'LATENCY_BUCKET_3: p50=' || round(p50_ms, 2) || 'ms p95=' || round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' || round(avg_ms, 2) || 'ms (n=' || num_queries || ')' as result
FROM benchmark_bucket(3);

\echo ''
\echo 'Token bucket 4 (4 search tokens):'
SELECT 'LATENCY_BUCKET_4: p50=' || round(p50_ms, 2) || 'ms p95=' || round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' || round(avg_ms, 2) || 'ms (n=' || num_queries || ')' as result
FROM benchmark_bucket(4);

\echo ''
\echo 'Token bucket 5 (5 search tokens):'
SELECT 'LATENCY_BUCKET_5: p50=' || round(p50_ms, 2) || 'ms p95=' || round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' || round(avg_ms, 2) || 'ms (n=' || num_queries || ')' as result
FROM benchmark_bucket(5);

\echo ''
\echo 'Token bucket 6 (6 search tokens):'
SELECT 'LATENCY_BUCKET_6: p50=' || round(p50_ms, 2) || 'ms p95=' || round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' || round(avg_ms, 2) || 'ms (n=' || num_queries || ')' as result
FROM benchmark_bucket(6);

\echo ''
\echo 'Token bucket 7 (7 search tokens):'
SELECT 'LATENCY_BUCKET_7: p50=' || round(p50_ms, 2) || 'ms p95=' || round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' || round(avg_ms, 2) || 'ms (n=' || num_queries || ')' as result
FROM benchmark_bucket(7);

\echo ''
\echo 'Token bucket 8 (8+ search tokens):'
SELECT 'LATENCY_BUCKET_8: p50=' || round(p50_ms, 2) || 'ms p95=' || round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' || round(avg_ms, 2) || 'ms (n=' || num_queries || ')' as result
FROM benchmark_bucket(8);

DROP FUNCTION benchmark_bucket;

-- ============================================================
-- Benchmark 2: Query Throughput (800 benchmark queries)
-- ============================================================
\echo ''
\echo '=== Benchmark 2: Query Throughput (800 queries, 3 iterations) ==='
\echo 'Running all 800 benchmark queries with warmup'

-- Helper function for throughput benchmark
CREATE OR REPLACE FUNCTION benchmark_throughput(iterations int DEFAULT 3)
RETURNS TABLE(median_ms numeric, min_ms numeric, max_ms numeric, queries_run int) AS $$
DECLARE
    q record;
    i int;
    start_ts timestamp;
    end_ts timestamp;
    times numeric[];
    sorted_times numeric[];
    query_count int;
BEGIN
    SELECT COUNT(*) INTO query_count FROM benchmark_queries;

    -- Warmup: run all queries once
    FOR q IN SELECT query_text FROM benchmark_queries ORDER BY query_id LOOP
        EXECUTE 'SELECT passage_id FROM msmarco_passages
                 ORDER BY passage_text <@> to_bm25query($1, ''msmarco_bm25_idx'')
                 LIMIT 10' USING q.query_text;
    END LOOP;

    -- Timed iterations
    times := ARRAY[]::numeric[];
    FOR i IN 1..iterations LOOP
        start_ts := clock_timestamp();
        FOR q IN SELECT query_text FROM benchmark_queries ORDER BY query_id LOOP
            EXECUTE 'SELECT passage_id FROM msmarco_passages
                     ORDER BY passage_text <@> to_bm25query($1, ''msmarco_bm25_idx'')
                     LIMIT 10' USING q.query_text;
        END LOOP;
        end_ts := clock_timestamp();
        times := array_append(times, EXTRACT(EPOCH FROM (end_ts - start_ts)) * 1000);
    END LOOP;

    SELECT array_agg(t ORDER BY t) INTO sorted_times FROM unnest(times) t;
    median_ms := sorted_times[(iterations + 1) / 2];
    min_ms := sorted_times[1];
    max_ms := sorted_times[iterations];
    queries_run := query_count;
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

SELECT 'Execution Time: ' || round(median_ms / queries_run, 3) || ' ms (min=' || round(min_ms / queries_run, 3) || ', max=' || round(max_ms / queries_run, 3) || ')' as result,
       'THROUGHPUT_RESULT: ' || queries_run || ' queries in ' || round(median_ms, 2) || ' ms (avg ' || round(median_ms / queries_run, 2) || ' ms/query)' as summary
FROM benchmark_throughput();

DROP FUNCTION benchmark_throughput;

-- ============================================================
-- Benchmark 3: Index Statistics
-- ============================================================
\echo ''
\echo '=== Benchmark 3: Index Statistics ==='

SELECT
    'msmarco_bm25_idx' as index_name,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx')) as index_size,
    pg_size_pretty(pg_relation_size('msmarco_passages')) as table_size,
    (SELECT COUNT(*) FROM msmarco_passages) as num_documents;

-- Cleanup
DROP TABLE benchmark_queries;

\echo ''
\echo '=== MS MARCO Query Benchmarks Complete ==='
