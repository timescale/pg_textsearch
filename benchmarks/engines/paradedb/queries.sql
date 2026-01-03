-- ParadeDB pg_search Query Benchmarks
-- Uses real MS-MARCO queries from the loaded dataset
-- Mirrors the pg_textsearch benchmark structure for comparison
--
-- Prerequisites:
--   - MS-MARCO data loaded
--   - ParadeDB index created via setup.sql
--
-- Usage:
--   psql -f queries.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== ParadeDB Query Benchmarks (Real Queries) ==='
\echo ''

-- Verify queries are loaded
DO $$
DECLARE
    cnt int;
BEGIN
    SELECT COUNT(*) INTO cnt FROM msmarco_queries;
    IF cnt = 0 THEN
        RAISE EXCEPTION 'No queries loaded. Run datasets/msmarco/load.sql first.';
    END IF;
    RAISE NOTICE 'Found % queries in msmarco_queries table', cnt;
END $$;

-- ============================================================
-- Warm-up: Run a few queries to ensure index is cached
-- ============================================================
\echo ''
\echo '=== Warming up ParadeDB index ==='
DO $$
DECLARE
    q text;
BEGIN
    FOR q IN SELECT query_text FROM msmarco_queries LIMIT 5 LOOP
        PERFORM passage_id FROM msmarco_passages
        WHERE passage_text ||| q
        ORDER BY pdb.score(passage_id) DESC
        LIMIT 10;
    END LOOP;
END $$;

-- ============================================================
-- Benchmark 1: Full Throughput Test
-- Run all dev queries and report aggregate statistics
-- ============================================================
\echo ''
\echo '=== Benchmark 1: Full Throughput Test (All Queries) ==='

DO $$
DECLARE
    q record;
    start_time timestamp;
    end_time timestamp;
    total_ms numeric;
    query_count int;
BEGIN
    SELECT COUNT(*) INTO query_count FROM msmarco_queries;

    start_time := clock_timestamp();

    FOR q IN SELECT query_id, query_text FROM msmarco_queries LOOP
        PERFORM passage_id FROM msmarco_passages
        WHERE passage_text ||| q.query_text
        ORDER BY pdb.score(passage_id) DESC
        LIMIT 10;
    END LOOP;

    end_time := clock_timestamp();
    total_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;

    RAISE NOTICE 'PARADEDB_THROUGHPUT_ALL: % queries in %.2f ms', query_count, total_ms;
    RAISE NOTICE 'PARADEDB_THROUGHPUT_AVG: %.3f ms/query', total_ms / query_count;
    RAISE NOTICE 'PARADEDB_THROUGHPUT_QPS: %.2f queries/sec',
        query_count / (total_ms / 1000.0);
END $$;

-- ============================================================
-- Benchmark 2: Query Length Analysis
-- Group queries by word count and measure performance
-- ============================================================
\echo ''
\echo '=== Benchmark 2: Query Length Analysis ==='

CREATE TEMP TABLE pdb_length_timing_results (
    length_bucket text,
    query_count int,
    total_ms numeric,
    avg_ms numeric
);

-- Short queries (1-2 words)
DO $$
DECLARE
    q record;
    start_time timestamp;
    end_time timestamp;
    total_ms numeric;
    query_count int := 0;
BEGIN
    start_time := clock_timestamp();

    FOR q IN
        SELECT query_id, query_text FROM msmarco_queries
        WHERE array_length(string_to_array(query_text, ' '), 1) <= 2
    LOOP
        PERFORM passage_id FROM msmarco_passages
        WHERE passage_text ||| q.query_text
        ORDER BY pdb.score(passage_id) DESC
        LIMIT 10;
        query_count := query_count + 1;
    END LOOP;

    end_time := clock_timestamp();
    total_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;

    IF query_count > 0 THEN
        INSERT INTO pdb_length_timing_results VALUES
            ('1-2 words', query_count, total_ms, total_ms / query_count);
    END IF;
END $$;

-- Medium queries (3-5 words)
DO $$
DECLARE
    q record;
    start_time timestamp;
    end_time timestamp;
    total_ms numeric;
    query_count int := 0;
BEGIN
    start_time := clock_timestamp();

    FOR q IN
        SELECT query_id, query_text FROM msmarco_queries
        WHERE array_length(string_to_array(query_text, ' '), 1) BETWEEN 3 AND 5
    LOOP
        PERFORM passage_id FROM msmarco_passages
        WHERE passage_text ||| q.query_text
        ORDER BY pdb.score(passage_id) DESC
        LIMIT 10;
        query_count := query_count + 1;
    END LOOP;

    end_time := clock_timestamp();
    total_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;

    IF query_count > 0 THEN
        INSERT INTO pdb_length_timing_results VALUES
            ('3-5 words', query_count, total_ms, total_ms / query_count);
    END IF;
END $$;

-- Long queries (6-8 words)
DO $$
DECLARE
    q record;
    start_time timestamp;
    end_time timestamp;
    total_ms numeric;
    query_count int := 0;
BEGIN
    start_time := clock_timestamp();

    FOR q IN
        SELECT query_id, query_text FROM msmarco_queries
        WHERE array_length(string_to_array(query_text, ' '), 1) BETWEEN 6 AND 8
    LOOP
        PERFORM passage_id FROM msmarco_passages
        WHERE passage_text ||| q.query_text
        ORDER BY pdb.score(passage_id) DESC
        LIMIT 10;
        query_count := query_count + 1;
    END LOOP;

    end_time := clock_timestamp();
    total_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;

    IF query_count > 0 THEN
        INSERT INTO pdb_length_timing_results VALUES
            ('6-8 words', query_count, total_ms, total_ms / query_count);
    END IF;
END $$;

-- Very long queries (9+ words)
DO $$
DECLARE
    q record;
    start_time timestamp;
    end_time timestamp;
    total_ms numeric;
    query_count int := 0;
BEGIN
    start_time := clock_timestamp();

    FOR q IN
        SELECT query_id, query_text FROM msmarco_queries
        WHERE array_length(string_to_array(query_text, ' '), 1) >= 9
    LOOP
        PERFORM passage_id FROM msmarco_passages
        WHERE passage_text ||| q.query_text
        ORDER BY pdb.score(passage_id) DESC
        LIMIT 10;
        query_count := query_count + 1;
    END LOOP;

    end_time := clock_timestamp();
    total_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;

    IF query_count > 0 THEN
        INSERT INTO pdb_length_timing_results VALUES
            ('9+ words', query_count, total_ms, total_ms / query_count);
    END IF;
END $$;

\echo ''
\echo 'ParadeDB Query Length Performance:'
SELECT
    length_bucket as "Length",
    query_count as "Count",
    round(total_ms::numeric, 2) || ' ms' as "Total Time",
    round(avg_ms::numeric, 3) || ' ms' as "Avg Time"
FROM pdb_length_timing_results
ORDER BY
    CASE length_bucket
        WHEN '1-2 words' THEN 1
        WHEN '3-5 words' THEN 2
        WHEN '6-8 words' THEN 3
        WHEN '9+ words' THEN 4
    END;

DROP TABLE pdb_length_timing_results;

-- ============================================================
-- Benchmark 3: Latency Profiling (Percentiles)
-- Sample queries for detailed latency distribution
-- ============================================================
\echo ''
\echo '=== Benchmark 3: Latency Profiling (100 Random Queries) ==='

CREATE TEMP TABLE pdb_query_latencies (
    query_id int,
    latency_ms numeric
);

DO $$
DECLARE
    q record;
    start_time timestamp;
    end_time timestamp;
    latency numeric;
BEGIN
    FOR q IN
        SELECT query_id, query_text FROM msmarco_queries
        ORDER BY random()
        LIMIT 100
    LOOP
        start_time := clock_timestamp();

        PERFORM passage_id FROM msmarco_passages
        WHERE passage_text ||| q.query_text
        ORDER BY pdb.score(passage_id) DESC
        LIMIT 10;

        end_time := clock_timestamp();
        latency := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;

        INSERT INTO pdb_query_latencies VALUES (q.query_id, latency);
    END LOOP;
END $$;

\echo ''
\echo 'ParadeDB Latency Distribution:'
SELECT
    round(min(latency_ms)::numeric, 3) as "Min (ms)",
    round(percentile_cont(0.50) WITHIN GROUP (ORDER BY latency_ms)::numeric, 3)
        as "P50 (ms)",
    round(percentile_cont(0.90) WITHIN GROUP (ORDER BY latency_ms)::numeric, 3)
        as "P90 (ms)",
    round(percentile_cont(0.95) WITHIN GROUP (ORDER BY latency_ms)::numeric, 3)
        as "P95 (ms)",
    round(percentile_cont(0.99) WITHIN GROUP (ORDER BY latency_ms)::numeric, 3)
        as "P99 (ms)",
    round(max(latency_ms)::numeric, 3) as "Max (ms)",
    round(avg(latency_ms)::numeric, 3) as "Avg (ms)"
FROM pdb_query_latencies;

DROP TABLE pdb_query_latencies;

-- ============================================================
-- Benchmark 4: Sample Query Results (with scores)
-- Show actual results for verification
-- ============================================================
\echo ''
\echo '=== Benchmark 4: Sample Query Results ==='

DO $$
DECLARE
    q record;
    r record;
BEGIN
    FOR q IN SELECT query_id, query_text FROM msmarco_queries
             ORDER BY random() LIMIT 3 LOOP
        RAISE NOTICE '';
        RAISE NOTICE 'Query %: "%"', q.query_id, q.query_text;
        RAISE NOTICE '  Top 3 results:';

        FOR r IN
            SELECT
                passage_id,
                round(pdb.score(passage_id)::numeric, 4) as score,
                left(passage_text, 60) || '...' as preview
            FROM msmarco_passages
            WHERE passage_text ||| q.query_text
            ORDER BY pdb.score(passage_id) DESC
            LIMIT 3
        LOOP
            RAISE NOTICE '    [%] score=% "%"',
                r.passage_id, r.score, r.preview;
        END LOOP;
    END LOOP;
END $$;

-- ============================================================
-- Summary Statistics
-- ============================================================
\echo ''
\echo '=== ParadeDB Index Statistics ==='

SELECT
    'msmarco_pdb_idx' as index_name,
    pg_size_pretty(pg_relation_size('msmarco_pdb_idx')) as index_size,
    pg_size_pretty(pg_relation_size('msmarco_passages')) as table_size,
    (SELECT COUNT(*) FROM msmarco_passages) as num_documents,
    (SELECT COUNT(*) FROM msmarco_queries) as num_queries;

\echo ''
\echo '=== ParadeDB Query Benchmarks Complete ==='
