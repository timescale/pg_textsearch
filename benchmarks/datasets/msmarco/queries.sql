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
-- Benchmark 1: Single Query Latency (using BM25 index scan)
-- ============================================================
\echo ''
\echo '=== Benchmark 1: Single Query Latency (BM25 Index Scan) ==='
\echo 'Running top-10 queries using the BM25 index'
\echo ''

-- Short query (1 word)
\echo 'Query 1: Short query (1 word) - "coffee"'
EXPLAIN (ANALYZE, TIMING, FORMAT TEXT)
SELECT passage_id,
       passage_text <@> to_bm25query('coffee', 'msmarco_bm25_idx') as score
FROM msmarco_passages
WHERE passage_text <@> to_bm25query('coffee', 'msmarco_bm25_idx') < 0
ORDER BY passage_text <@> to_bm25query('coffee', 'msmarco_bm25_idx')
LIMIT 10;

\echo ''
\echo 'Query 2: Medium query (3 words) - "how to cook"'
EXPLAIN (ANALYZE, TIMING, FORMAT TEXT)
SELECT passage_id,
       passage_text <@> to_bm25query('how to cook', 'msmarco_bm25_idx') as score
FROM msmarco_passages
WHERE passage_text <@> to_bm25query('how to cook', 'msmarco_bm25_idx') < 0
ORDER BY passage_text <@> to_bm25query('how to cook', 'msmarco_bm25_idx')
LIMIT 10;

\echo ''
\echo 'Query 3: Long query (question) - "what is the capital of france"'
EXPLAIN (ANALYZE, TIMING, FORMAT TEXT)
SELECT passage_id,
       passage_text <@> to_bm25query('what is the capital of france',
                                     'msmarco_bm25_idx') as score
FROM msmarco_passages
WHERE passage_text <@> to_bm25query('what is the capital of france',
                                    'msmarco_bm25_idx') < 0
ORDER BY passage_text <@> to_bm25query('what is the capital of france',
                                       'msmarco_bm25_idx')
LIMIT 10;

\echo ''
\echo 'Query 4: Common term - "the"'
EXPLAIN (ANALYZE, TIMING, FORMAT TEXT)
SELECT passage_id,
       passage_text <@> to_bm25query('the', 'msmarco_bm25_idx') as score
FROM msmarco_passages
WHERE passage_text <@> to_bm25query('the', 'msmarco_bm25_idx') < 0
ORDER BY passage_text <@> to_bm25query('the', 'msmarco_bm25_idx')
LIMIT 10;

\echo ''
\echo 'Query 5: Rare term - "cryptocurrency blockchain"'
EXPLAIN (ANALYZE, TIMING, FORMAT TEXT)
SELECT passage_id,
       passage_text <@> to_bm25query('cryptocurrency blockchain',
                                     'msmarco_bm25_idx') as score
FROM msmarco_passages
WHERE passage_text <@> to_bm25query('cryptocurrency blockchain',
                                    'msmarco_bm25_idx') < 0
ORDER BY passage_text <@> to_bm25query('cryptocurrency blockchain',
                                       'msmarco_bm25_idx')
LIMIT 10;

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
    cnt int;
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
    RAISE NOTICE 'THROUGHPUT_RESULT: 20 queries in % ms (avg % ms/query)',
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
