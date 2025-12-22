-- MS MARCO Passage Ranking - Query Benchmarks
-- Runs various query workloads against the indexed MS MARCO collection
-- Outputs structured timing data for historical tracking

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO Query Benchmarks ==='
\echo ''

-- Warm up: run a few queries to ensure index is cached
\echo 'Warming up index...'
SELECT passage_id FROM msmarco_passages
WHERE passage_text <@> to_bm25query('test', 'msmarco_bm25_idx') < 0
ORDER BY passage_text <@> to_bm25query('test', 'msmarco_bm25_idx')
LIMIT 10;

SELECT passage_id FROM msmarco_passages
WHERE passage_text <@> to_bm25query('example query', 'msmarco_bm25_idx') < 0
ORDER BY passage_text <@> to_bm25query('example query', 'msmarco_bm25_idx')
LIMIT 10;

-- ============================================================
-- Benchmark 1: Query Latency (10 iterations each, median reported)
-- ============================================================
\echo ''
\echo '=== Benchmark 1: Query Latency (10 iterations each) ==='
\echo 'Running top-10 queries using the BM25 index'
\echo ''

-- Helper function to run a query multiple times and return median execution time
-- When with_score=true, also SELECTs the score (tests per-row operator call)
CREATE OR REPLACE FUNCTION benchmark_query(query_text text, iterations int DEFAULT 10, with_score boolean DEFAULT false)
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
        IF with_score THEN
            -- Query that returns scores (calls operator per row)
            EXECUTE 'SELECT passage_id, passage_text <@> to_bm25query($1, ''msmarco_bm25_idx'') as score
                     FROM msmarco_passages
                     WHERE passage_text <@> to_bm25query($1, ''msmarco_bm25_idx'') < 0
                     ORDER BY passage_text <@> to_bm25query($1, ''msmarco_bm25_idx'')
                     LIMIT 10' USING query_text;
        ELSE
            -- Query that only orders by score (no per-row operator call)
            EXECUTE 'SELECT passage_id FROM msmarco_passages
                     WHERE passage_text <@> to_bm25query($1, ''msmarco_bm25_idx'') < 0
                     ORDER BY passage_text <@> to_bm25query($1, ''msmarco_bm25_idx'')
                     LIMIT 10' USING query_text;
        END IF;
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

-- ============================================================
-- Part A: Queries WITHOUT scores (order by score, don't return it)
-- ============================================================
\echo ''
\echo '--- Part A: Queries WITHOUT scores (index scan only) ---'

-- Short query (1 word)
\echo 'Query 1a: Short query (1 word) - "coffee" [no score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('coffee', 10, false);

\echo ''
\echo 'Query 2a: Medium query (3 words) - "how to cook" [no score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('how to cook', 10, false);

\echo ''
\echo 'Query 3a: Long query (question) - "what is the capital of france" [no score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('what is the capital of france', 10, false);

\echo ''
\echo 'Query 4a: Common term - "the" [no score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('the', 10, false);

\echo ''
\echo 'Query 5a: Rare term - "cryptocurrency blockchain" [no score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('cryptocurrency blockchain', 10, false);

-- ============================================================
-- Part B: Queries WITH scores (per-row operator call)
-- ============================================================
\echo ''
\echo '--- Part B: Queries WITH scores (per-row operator) ---'

\echo ''
\echo 'Query 1b: Short query (1 word) - "coffee" [with score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('coffee', 10, true);

\echo ''
\echo 'Query 2b: Medium query (3 words) - "how to cook" [with score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('how to cook', 10, true);

\echo ''
\echo 'Query 3b: Long query (question) - "what is the capital of france" [with score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('what is the capital of france', 10, true);

\echo ''
\echo 'Query 4b: Common term - "the" [with score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('the', 10, true);

\echo ''
\echo 'Query 5b: Rare term - "cryptocurrency blockchain" [with score]'
SELECT 'Execution Time: ' || round(median_ms, 3) || ' ms (min=' || round(min_ms, 3) || ', max=' || round(max_ms, 3) || ')' as result
FROM benchmark_query('cryptocurrency blockchain', 10, true);

DROP FUNCTION benchmark_query;

-- ============================================================
-- Benchmark 2: Query Throughput (batch of queries)
-- ============================================================
\echo ''
\echo '=== Benchmark 2: Query Throughput (20 queries x 2 modes) ==='

-- Time 20 queries in sequence - WITHOUT scores
\echo 'Running 20 sequential top-10 queries [no score]'
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
        PERFORM passage_id FROM msmarco_passages
        WHERE passage_text <@> to_bm25query(q, 'msmarco_bm25_idx') < 0
        ORDER BY passage_text <@> to_bm25query(q, 'msmarco_bm25_idx')
        LIMIT 10;
    END LOOP;
    end_time := clock_timestamp();
    total_ms := EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
    RAISE NOTICE 'THROUGHPUT_RESULT_NO_SCORE: 20 queries in % ms (avg % ms/query)',
        round(total_ms::numeric, 2), round((total_ms / 20)::numeric, 2);
END $$;

-- Time 20 queries in sequence - WITH scores
\echo ''
\echo 'Running 20 sequential top-10 queries [with score]'
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
    dummy record;
BEGIN
    start_time := clock_timestamp();
    FOREACH q IN ARRAY queries LOOP
        -- Need to actually retrieve results to ensure score computation happens
        FOR dummy IN
            SELECT passage_id, passage_text <@> to_bm25query(q, 'msmarco_bm25_idx') as score
            FROM msmarco_passages
            WHERE passage_text <@> to_bm25query(q, 'msmarco_bm25_idx') < 0
            ORDER BY passage_text <@> to_bm25query(q, 'msmarco_bm25_idx')
            LIMIT 10
        LOOP
            -- Just iterate to force score computation
        END LOOP;
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
    'msmarco_bm25_idx' as index_name,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx')) as index_size,
    pg_size_pretty(pg_relation_size('msmarco_passages')) as table_size,
    (SELECT COUNT(*) FROM msmarco_passages) as num_documents;

-- ============================================================
-- Summary: Extract key metrics for historical tracking
-- ============================================================
\echo ''
\echo '=== BENCHMARK SUMMARY (for historical tracking) ==='
\echo 'Format: METRIC_NAME: value unit'

-- Re-run key queries and extract execution time only
\echo ''
\o /dev/null
SELECT passage_id FROM msmarco_passages
WHERE passage_text <@> to_bm25query('coffee', 'msmarco_bm25_idx') < 0
ORDER BY passage_text <@> to_bm25query('coffee', 'msmarco_bm25_idx')
LIMIT 10;
\o
\echo 'See EXPLAIN ANALYZE output above for per-query latencies'

\echo ''
\echo '=== MS MARCO Query Benchmarks Complete ==='
