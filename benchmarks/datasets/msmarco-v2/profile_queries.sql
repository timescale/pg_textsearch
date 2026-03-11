-- Profile slow queries (5-8+ token buckets) to find optimization targets.
-- Outputs per-query wall-clock timing + BMW stats from the server log.
--
-- Usage:
--   SET pg_textsearch.log_bmw_stats = true;
--   SET log_min_messages = LOG;
--   \i benchmarks/datasets/msmarco-v2/profile_queries.sql
--
-- Then grep the PG log for "BMW stats" lines to correlate with output.

\set ON_ERROR_STOP on
\timing on

-- Enable BMW diagnostics
SET pg_textsearch.log_bmw_stats = true;
SET client_min_messages = LOG;

-- Load benchmark queries
DROP TABLE IF EXISTS benchmark_queries;
CREATE TABLE benchmark_queries (
    query_id INTEGER,
    query_text TEXT,
    token_bucket INTEGER
);
\copy benchmark_queries FROM 'benchmarks/datasets/msmarco-v2/benchmark_queries.tsv' WITH (FORMAT text, DELIMITER E'\t')

-- Warm up: touch each bucket once
DO $$
DECLARE
    q record;
BEGIN
    FOR q IN
        SELECT DISTINCT ON (token_bucket) query_text
        FROM benchmark_queries
        WHERE token_bucket >= 5
        ORDER BY token_bucket, query_id
    LOOP
        EXECUTE 'SELECT passage_id FROM msmarco_v2_passages
                 ORDER BY passage_text <@> to_bm25query($1,
                     ''msmarco_v2_bm25_idx'') LIMIT 10'
            USING q.query_text;
    END LOOP;
END;
$$;

\echo ''
\echo '=== Profiling Buckets 5-8 ==='

-- Per-query timing with BMW stats (logged server-side)
CREATE TEMP TABLE profile_results (
    query_id    int,
    token_bucket int,
    query_text  text,
    elapsed_ms  numeric,
    num_results int
);

DO $$
DECLARE
    q record;
    t0 timestamp;
    t1 timestamp;
    cnt int;
BEGIN
    FOR q IN
        SELECT query_id, query_text, token_bucket
        FROM benchmark_queries
        WHERE token_bucket >= 5
        ORDER BY token_bucket, query_id
    LOOP
        t0 := clock_timestamp();
        EXECUTE 'SELECT COUNT(*) FROM (
            SELECT passage_id FROM msmarco_v2_passages
            ORDER BY passage_text <@> to_bm25query($1,
                ''msmarco_v2_bm25_idx'') LIMIT 10
        ) t' INTO cnt USING q.query_text;
        t1 := clock_timestamp();

        INSERT INTO profile_results VALUES (
            q.query_id, q.token_bucket, q.query_text,
            EXTRACT(EPOCH FROM (t1 - t0)) * 1000, cnt
        );
    END LOOP;
END;
$$;

-- Summary by bucket
\echo ''
\echo '=== Per-Bucket Summary ==='
SELECT
    token_bucket,
    COUNT(*) AS n,
    round(percentile_cont(0.50) WITHIN GROUP
        (ORDER BY elapsed_ms)::numeric, 2) AS p50_ms,
    round(percentile_cont(0.95) WITHIN GROUP
        (ORDER BY elapsed_ms)::numeric, 2) AS p95_ms,
    round(AVG(elapsed_ms), 2) AS avg_ms,
    round(MIN(elapsed_ms), 2) AS min_ms,
    round(MAX(elapsed_ms), 2) AS max_ms
FROM profile_results
GROUP BY token_bucket
ORDER BY token_bucket;

-- Slowest 20 queries
\echo ''
\echo '=== Slowest 20 Queries ==='
SELECT
    query_id,
    token_bucket AS bucket,
    round(elapsed_ms, 2) AS ms,
    LEFT(query_text, 70) AS query
FROM profile_results
ORDER BY elapsed_ms DESC
LIMIT 20;

-- Distribution: how many queries in each latency band?
\echo ''
\echo '=== Latency Distribution (buckets 5-8) ==='
SELECT
    CASE
        WHEN elapsed_ms < 100  THEN '  <100ms'
        WHEN elapsed_ms < 200  THEN ' 100-200ms'
        WHEN elapsed_ms < 300  THEN ' 200-300ms'
        WHEN elapsed_ms < 400  THEN ' 300-400ms'
        WHEN elapsed_ms < 500  THEN ' 400-500ms'
        WHEN elapsed_ms < 750  THEN ' 500-750ms'
        ELSE                        ' 750ms+'
    END AS band,
    COUNT(*) AS n,
    round(100.0 * COUNT(*) / SUM(COUNT(*)) OVER (), 1) AS pct
FROM profile_results
GROUP BY 1
ORDER BY 1;

-- Token count vs elapsed correlation
\echo ''
\echo '=== Per-Query Detail (buckets 5-8) ==='
SELECT
    query_id,
    token_bucket AS bucket,
    round(elapsed_ms, 2) AS ms,
    query_text
FROM profile_results
ORDER BY token_bucket, elapsed_ms DESC;

DROP TABLE profile_results;
DROP TABLE benchmark_queries;

\echo ''
\echo '=== Profiling Complete ==='
\echo 'Check PG log for BMW stats: grep "BMW stats" <logfile>'
