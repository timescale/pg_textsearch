-- MS MARCO Passage Ranking - Query Benchmarks (ParadeDB)
-- Runs various query workloads against the indexed MS MARCO collection using ParadeDB
-- Outputs structured timing data for historical tracking

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO Query Benchmarks (ParadeDB) ==='
\echo ''

-- Warm up: run a few queries to ensure index is cached
\echo 'Warming up index...'
SELECT passage_id FROM msmarco_passages_paradedb
WHERE passage_text @@@ 'test'
LIMIT 10;

SELECT passage_id FROM msmarco_passages_paradedb
WHERE passage_text @@@ 'example query'
LIMIT 10;

-- ============================================================
-- Benchmark 1: Query Latency (10 iterations each, median reported)
-- ============================================================
\echo ''
\echo '=== Benchmark 1: Query Latency (10 iterations each) ==='
\echo 'Running top-10 queries using the ParadeDB BM25 index'
\echo ''

-- Helper function to run a query multiple times and return median execution time
CREATE OR REPLACE FUNCTION benchmark_query_paradedb(query_text text, iterations int DEFAULT 10)
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
        EXECUTE 'SELECT passage_id FROM msmarco_passages_paradedb
                 WHERE passage_text @@@ $1
                 ORDER BY paradedb.score(passage_id) DESC
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

-- Short query (1 word)
\echo 'Query 1: Short query (1 word) - "coffee"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb('coffee');

\echo ''
\echo 'Query 2: Medium query (3 words) - "how to cook"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb('how to cook');

\echo ''
\echo 'Query 3: Long query (question) - "what is the capital of france"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb('what is the capital of france');

\echo ''
\echo 'Query 4: Common term - "the"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb('the');

\echo ''
\echo 'Query 5: Rare term - "cryptocurrency blockchain"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb('cryptocurrency blockchain');

DROP FUNCTION benchmark_query_paradedb;

-- ============================================================
-- Benchmark 1b: Query Latency WITH SCORE (10 iterations each)
-- ============================================================
\echo ''
\echo '=== Benchmark 1b: Query Latency WITH SCORE (10 iterations each) ==='
\echo 'Running top-10 queries with score in SELECT clause'
\echo ''

-- Helper function for queries that return score
CREATE OR REPLACE FUNCTION benchmark_query_paradedb_with_score(query_text text, iterations int DEFAULT 10)
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
        EXECUTE 'SELECT passage_id, paradedb.score(passage_id) AS score
                 FROM msmarco_passages_paradedb
                 WHERE passage_text @@@ $1
                 ORDER BY score DESC
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

-- Short query (1 word) with score
\echo 'Query 1 (with score): Short query (1 word) - "coffee"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb_with_score('coffee');

\echo ''
\echo 'Query 2 (with score): Medium query (3 words) - "how to cook"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb_with_score('how to cook');

\echo ''
\echo 'Query 3 (with score): Long query (question) - "what is the capital of france"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb_with_score('what is the capital of france');

\echo ''
\echo 'Query 4 (with score): Common term - "the"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb_with_score('the');

\echo ''
\echo 'Query 5 (with score): Rare term - "cryptocurrency blockchain"'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query_paradedb_with_score('cryptocurrency blockchain');

DROP FUNCTION benchmark_query_paradedb_with_score;

-- ============================================================
-- Benchmark 2: Query Throughput (batch of queries)
-- ============================================================
\echo ''
\echo '=== Benchmark 2: Query Throughput (20 queries) ==='
\echo 'Running 20 sequential top-10 queries'

-- Time 20 queries in sequence
DO $$
DECLARE
    queries text[] := ARRAY[
        'weather forecast', 'stock market', 'recipe chicken',
        'python programming', 'machine learning', 'climate change',
        'health benefits', 'travel destinations', 'movie reviews',
        'sports scores', 'music history', 'science experiments',
        'cooking tips', 'fitness exercises', 'book recommendations',
        'technology news', 'financial advice', 'home improvement',
        'garden plants', 'pet care'
    ];
    q text;
    start_time timestamp;
    end_time timestamp;
    total_ms numeric := 0;
BEGIN
    start_time := clock_timestamp();
    FOREACH q IN ARRAY queries LOOP
        PERFORM passage_id FROM msmarco_passages_paradedb
        WHERE passage_text @@@ q
        ORDER BY paradedb.score(passage_id) DESC
        LIMIT 10;
    END LOOP;
    end_time := clock_timestamp();
    total_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    RAISE NOTICE 'THROUGHPUT_RESULT: 20 queries in % ms (avg % ms/query)',
        round(total_ms::numeric, 2), round((total_ms / 20)::numeric, 2);
END $$;

-- ============================================================
-- Benchmark 2b: Query Throughput WITH SCORE (batch of queries)
-- ============================================================
\echo ''
\echo '=== Benchmark 2b: Query Throughput WITH SCORE (20 queries) ==='
\echo 'Running 20 sequential top-10 queries with score in SELECT'

-- Time 20 queries in sequence with score
DO $$
DECLARE
    queries text[] := ARRAY[
        'weather forecast', 'stock market', 'recipe chicken',
        'python programming', 'machine learning', 'climate change',
        'health benefits', 'travel destinations', 'movie reviews',
        'sports scores', 'music history', 'science experiments',
        'cooking tips', 'fitness exercises', 'book recommendations',
        'technology news', 'financial advice', 'home improvement',
        'garden plants', 'pet care'
    ];
    q text;
    start_time timestamp;
    end_time timestamp;
    total_ms numeric := 0;
BEGIN
    start_time := clock_timestamp();
    FOREACH q IN ARRAY queries LOOP
        PERFORM passage_id, paradedb.score(passage_id) AS score
        FROM msmarco_passages_paradedb
        WHERE passage_text @@@ q
        ORDER BY score DESC
        LIMIT 10;
    END LOOP;
    end_time := clock_timestamp();
    total_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    RAISE NOTICE 'THROUGHPUT_RESULT_WITH_SCORE: 20 queries in % ms (avg % ms/query)',
        round(total_ms::numeric, 2), round((total_ms / 20)::numeric, 2);
END $$;

-- ============================================================
-- Benchmark 3: Index Statistics
-- ============================================================
\echo ''
\echo '=== Benchmark 3: Index Statistics ==='

SELECT
    'msmarco_paradedb_idx' as index_name,
    pg_size_pretty(pg_relation_size('msmarco_paradedb_idx')) as index_size,
    pg_size_pretty(pg_relation_size('msmarco_passages_paradedb')) as table_size,
    (SELECT COUNT(*) FROM msmarco_passages_paradedb) as num_documents;

\echo ''
\echo '=== MS MARCO Query Benchmarks Complete (ParadeDB) ==='
