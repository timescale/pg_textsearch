-- Cranfield Collection BM25 Benchmark - Query Performance
-- Tests BM25 index scan performance using the standard Cranfield IR collection
-- Outputs structured timing data for regression detection

\set ON_ERROR_STOP on
\timing on

\echo '=== Cranfield BM25 Benchmark - Query Phase ==='
\echo ''

-- Warm up: run a few queries to ensure index is cached
\echo 'Warming up index...'
SELECT doc_id FROM cranfield_full_documents
ORDER BY full_text <@> to_bm25query('boundary layer', 'cranfield_full_tapir_idx')
LIMIT 10;

SELECT doc_id FROM cranfield_full_documents
ORDER BY full_text <@> to_bm25query('heat transfer', 'cranfield_full_tapir_idx')
LIMIT 10;

-- ============================================================
-- Benchmark 1: Query Latency (10 iterations each, median reported)
-- ============================================================
\echo ''
\echo '=== Benchmark 1: Query Latency (10 iterations each) ==='
\echo 'Running top-10 queries using the BM25 index'
\echo ''

-- Helper function to run a query multiple times and return median execution time
CREATE OR REPLACE FUNCTION benchmark_query(query_text text, iterations int DEFAULT 10)
RETURNS TABLE(median_ms numeric, min_ms numeric, max_ms numeric) AS $$
DECLARE
    i int;
    start_ts timestamp;
    end_ts timestamp;
    times numeric[];
    sorted_times numeric[];
BEGIN
    times := ARRAY[]::numeric[];
    FOR i IN 1..iterations LOOP
        start_ts := clock_timestamp();
        EXECUTE 'SELECT doc_id FROM cranfield_full_documents
                 ORDER BY full_text <@> to_bm25query($1, ''cranfield_full_tapir_idx'')
                 LIMIT 10' USING query_text;
        end_ts := clock_timestamp();
        times := array_append(times, EXTRACT(EPOCH FROM (end_ts - start_ts)) * 1000);
    END LOOP;
    SELECT array_agg(t ORDER BY t) INTO sorted_times FROM unnest(times) t;
    median_ms := sorted_times[(iterations + 1) / 2];
    min_ms := sorted_times[1];
    max_ms := sorted_times[iterations];
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- Short query (2 words)
\echo 'Query 1: Short query (2 words) - "boundary layer"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('boundary layer');

\echo ''
\echo 'Query 2: Medium query (4 words) - "supersonic flow heat transfer"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('supersonic flow heat transfer');

\echo ''
\echo 'Query 3: Long query (from Cranfield query 1)'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('what similarity laws must be obeyed when constructing aeroelastic models of heated high speed aircraft');

\echo ''
\echo 'Query 4: Common terms - "flow pressure"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('flow pressure');

\echo ''
\echo 'Query 5: Specific terms - "magnetohydrodynamic viscosity"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('magnetohydrodynamic viscosity');

DROP FUNCTION benchmark_query;

-- ============================================================
-- Benchmark 2: Query Throughput (all 225 Cranfield queries)
-- ============================================================
\echo ''
\echo '=== Benchmark 2: Query Throughput (225 Cranfield queries) ==='
\echo 'Running all standard Cranfield queries sequentially'

DO $$
DECLARE
    q RECORD;
    start_time timestamp;
    end_time timestamp;
    total_ms numeric := 0;
    query_count int := 0;
BEGIN
    start_time := clock_timestamp();
    FOR q IN SELECT query_id, query_text FROM cranfield_full_queries ORDER BY query_id LOOP
        PERFORM doc_id FROM cranfield_full_documents
        ORDER BY full_text <@> to_bm25query(q.query_text, 'cranfield_full_tapir_idx')
        LIMIT 10;
        query_count := query_count + 1;
    END LOOP;
    end_time := clock_timestamp();
    total_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    RAISE NOTICE 'THROUGHPUT_RESULT: % queries in % ms (avg % ms/query)',
        query_count, round(total_ms::numeric, 2), round((total_ms / query_count)::numeric, 2);
END $$;

-- ============================================================
-- Benchmark 3: Index Statistics
-- ============================================================
\echo ''
\echo '=== Benchmark 3: Index Statistics ==='

SELECT
    'cranfield_full_tapir_idx' as index_name,
    pg_size_pretty(pg_relation_size('cranfield_full_tapir_idx')) as index_size,
    pg_size_pretty(pg_relation_size('cranfield_full_documents')) as table_size,
    (SELECT COUNT(*) FROM cranfield_full_documents) as num_documents;

-- ============================================================
-- Benchmark 4: Search Quality Validation (Precision@10)
-- ============================================================
\echo ''
\echo '=== Benchmark 4: Search Quality Validation ==='
\echo 'Checking precision@10 against Cranfield relevance judgments'

WITH query_results AS (
    SELECT
        q.query_id,
        d.doc_id,
        ROW_NUMBER() OVER (
            PARTITION BY q.query_id
            ORDER BY d.full_text <@> to_bm25query(q.query_text, 'cranfield_full_tapir_idx')
        ) as rank
    FROM cranfield_full_queries q
    CROSS JOIN LATERAL (
        SELECT doc_id, full_text
        FROM cranfield_full_documents
        ORDER BY full_text <@> to_bm25query(q.query_text, 'cranfield_full_tapir_idx')
        LIMIT 10
    ) d
    WHERE q.query_id <= 10  -- Sample first 10 queries for validation
),
precision_calc AS (
    SELECT
        qr.query_id,
        COUNT(CASE WHEN er.doc_id IS NOT NULL THEN 1 END)::float / 10.0 as precision_at_10
    FROM query_results qr
    LEFT JOIN cranfield_full_expected_rankings er
        ON qr.query_id = er.query_id AND qr.doc_id = er.doc_id
    WHERE qr.rank <= 10
    GROUP BY qr.query_id
)
SELECT
    'Mean Precision@10 (queries 1-10)' as metric,
    round(avg(precision_at_10)::numeric, 4) as value
FROM precision_calc;

\echo ''
\echo '=== Cranfield BM25 Benchmark Complete ==='
