-- Wikipedia Dataset - Query Benchmarks
-- Runs various query workloads against the indexed Wikipedia collection

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia Query Benchmarks ==='
\echo ''

-- Get dataset size
SELECT 'Dataset size: ' || COUNT(*) || ' articles' as info FROM wikipedia_articles;

-- Warm up
\echo 'Warming up index...'
SELECT COUNT(*) FROM wikipedia_articles
WHERE content <@> to_bm25query('test', 'wikipedia_bm25_idx') < 0;

-- Benchmark 1: Single-word queries
\echo ''
\echo '=== Benchmark 1: Single-Word Queries ==='

\echo 'Query: "algorithm"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('algorithm', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

\echo ''
\echo 'Query: "history"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('history', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

\echo ''
\echo 'Query: "science"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('science', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

-- Benchmark 2: Multi-word queries
\echo ''
\echo '=== Benchmark 2: Multi-Word Queries ==='

\echo 'Query: "machine learning"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('machine learning', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

\echo ''
\echo 'Query: "world war history"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('world war history', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

\echo ''
\echo 'Query: "climate change effects environment"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('climate change effects environment', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

-- Benchmark 3: Question-style queries
\echo ''
\echo '=== Benchmark 3: Question-Style Queries ==='

\echo 'Query: "how does photosynthesis work"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('how does photosynthesis work', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

\echo ''
\echo 'Query: "what causes earthquakes"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('what causes earthquakes', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

-- Benchmark 4: Rare term queries
\echo ''
\echo '=== Benchmark 4: Rare Term Queries ==='

\echo 'Query: "mitochondria"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('mitochondria', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

\echo ''
\echo 'Query: "Constantinople Byzantine Empire"'
EXPLAIN ANALYZE
SELECT article_id, title,
       content <@> to_bm25query('Constantinople Byzantine Empire', 'wikipedia_bm25_idx') as score
FROM wikipedia_articles
ORDER BY score
LIMIT 10;

-- Benchmark 5: Batch query processing
\echo ''
\echo '=== Benchmark 5: Batch Query Processing ==='
\echo 'Running 20 different queries sequentially...'

DO $$
DECLARE
    queries text[] := ARRAY[
        'computer', 'biology', 'mathematics', 'physics', 'chemistry',
        'geography', 'philosophy', 'economics', 'psychology', 'medicine',
        'engineering', 'astronomy', 'geology', 'linguistics', 'anthropology',
        'artificial intelligence', 'quantum mechanics', 'natural selection',
        'industrial revolution', 'renaissance art'
    ];
    q text;
    start_time timestamp;
    total_ms numeric := 0;
    result_count integer;
BEGIN
    start_time := clock_timestamp();

    FOREACH q IN ARRAY queries LOOP
        EXECUTE format(
            'SELECT COUNT(*) FROM (
                SELECT article_id FROM wikipedia_articles
                WHERE content <@> to_bm25query(%L, ''wikipedia_bm25_idx'') < 0
                LIMIT 10
            ) t',
            q
        ) INTO result_count;
    END LOOP;

    total_ms := EXTRACT(EPOCH FROM (clock_timestamp() - start_time)) * 1000;
    RAISE NOTICE 'Total time for 20 queries: % ms', round(total_ms, 1);
    RAISE NOTICE 'Average query time: % ms', round(total_ms / 20, 1);
END $$;

-- Benchmark 6: Result count queries (no LIMIT)
\echo ''
\echo '=== Benchmark 6: Full Result Count ==='
\echo 'Counting all matching documents (no LIMIT)'

\echo 'Query: "the" (very common term)'
EXPLAIN ANALYZE
SELECT COUNT(*)
FROM wikipedia_articles
WHERE content <@> to_bm25query('the', 'wikipedia_bm25_idx') < 0;

\echo ''
\echo 'Query: "quantum" (moderately rare)'
EXPLAIN ANALYZE
SELECT COUNT(*)
FROM wikipedia_articles
WHERE content <@> to_bm25query('quantum', 'wikipedia_bm25_idx') < 0;

\echo ''
\echo '=== Wikipedia Query Benchmarks Complete ==='
