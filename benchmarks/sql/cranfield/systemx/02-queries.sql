-- Cranfield Collection BM25 Benchmark - Query Performance (System X)
-- Tests BM25 index scan performance using the standard Cranfield IR collection
-- Outputs structured timing data for regression detection

\set ON_ERROR_STOP on
\timing on

\echo '=== Cranfield BM25 Benchmark - Query Phase (System X) ==='
\echo ''

-- Warm up: run a few queries to ensure index is cached
\echo 'Warming up index...'
SELECT doc_id FROM cranfield_systemx_documents
WHERE full_text @@@ paradedb.match('full_text', 'boundary layer')
ORDER BY paradedb.score(doc_id) DESC
LIMIT 10;

SELECT doc_id FROM cranfield_systemx_documents
WHERE full_text @@@ paradedb.match('full_text', 'heat transfer')
ORDER BY paradedb.score(doc_id) DESC
LIMIT 10;

-- ============================================================
-- Benchmark 1: Query Latency (10 iterations each, median reported)
-- ============================================================
\echo ''
\echo '=== Benchmark 1: Query Latency (10 iterations each) ==='
\echo 'Running top-10 queries using the BM25 index'
\echo ''

-- Helper function to run a query multiple times and return median execution time
CREATE OR REPLACE FUNCTION benchmark_query_systemx(query_text text, iterations int DEFAULT 10)
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
        EXECUTE 'SELECT doc_id FROM cranfield_systemx_documents
                 WHERE full_text @@@ paradedb.match(''full_text'', $1)
                 ORDER BY paradedb.score(doc_id) DESC
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
FROM benchmark_query_systemx('boundary layer');

\echo ''
\echo 'Query 2: Medium query (4 words) - "supersonic flow heat transfer"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_systemx('supersonic flow heat transfer');

\echo ''
\echo 'Query 3: Long query (from Cranfield query 1)'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_systemx('what similarity laws must be obeyed when constructing aeroelastic models of heated high speed aircraft');

\echo ''
\echo 'Query 4: Common terms - "flow pressure"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_systemx('flow pressure');

\echo ''
\echo 'Query 5: Specific terms - "magnetohydrodynamic viscosity"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_systemx('magnetohydrodynamic viscosity');

DROP FUNCTION benchmark_query_systemx;

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
    FOR q IN SELECT query_id, query_text FROM cranfield_systemx_queries ORDER BY query_id LOOP
        PERFORM doc_id FROM cranfield_systemx_documents
        WHERE full_text @@@ paradedb.match('full_text', q.query_text)
        ORDER BY paradedb.score(doc_id) DESC
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
    'cranfield_systemx_idx' as index_name,
    pg_size_pretty(pg_relation_size('cranfield_systemx_idx')) as index_size,
    pg_size_pretty(pg_relation_size('cranfield_systemx_documents')) as table_size,
    (SELECT COUNT(*) FROM cranfield_systemx_documents) as num_documents;

\echo ''
\echo '=== Cranfield BM25 Benchmark Complete (System X) ==='
