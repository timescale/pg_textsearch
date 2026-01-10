-- Wikipedia Dataset - Query Benchmarks (System X)
-- Runs various query workloads against the indexed Wikipedia collection

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia Query Benchmarks (System X) ==='
\echo ''

-- Get dataset size
SELECT 'Dataset size: ' || COUNT(*) || ' articles' as info FROM wikipedia_articles_systemx;

-- Warm up
\echo 'Warming up index...'
SELECT article_id FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'test')
ORDER BY paradedb.score(article_id) DESC
LIMIT 10;

-- Benchmark 1: Single-word queries
\echo ''
\echo '=== Benchmark 1: Single-Word Queries ==='

\echo 'Query: "algorithm"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'algorithm')
ORDER BY score DESC
LIMIT 10;

\echo ''
\echo 'Query: "history"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'history')
ORDER BY score DESC
LIMIT 10;

\echo ''
\echo 'Query: "science"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'science')
ORDER BY score DESC
LIMIT 10;

-- Benchmark 2: Multi-word queries
\echo ''
\echo '=== Benchmark 2: Multi-Word Queries ==='

\echo 'Query: "machine learning"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'machine learning')
ORDER BY score DESC
LIMIT 10;

\echo ''
\echo 'Query: "world war history"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'world war history')
ORDER BY score DESC
LIMIT 10;

\echo ''
\echo 'Query: "climate change effects environment"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'climate change effects environment')
ORDER BY score DESC
LIMIT 10;

-- Benchmark 3: Question-style queries
\echo ''
\echo '=== Benchmark 3: Question-Style Queries ==='

\echo 'Query: "how does photosynthesis work"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'how does photosynthesis work')
ORDER BY score DESC
LIMIT 10;

\echo ''
\echo 'Query: "what causes earthquakes"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'what causes earthquakes')
ORDER BY score DESC
LIMIT 10;

-- Benchmark 4: Rare term queries
\echo ''
\echo '=== Benchmark 4: Rare Term Queries ==='

\echo 'Query: "mitochondria"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'mitochondria')
ORDER BY score DESC
LIMIT 10;

\echo ''
\echo 'Query: "Constantinople Byzantine Empire"'
EXPLAIN ANALYZE
SELECT article_id, title,
       paradedb.score(article_id) as score
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'Constantinople Byzantine Empire')
ORDER BY score DESC
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
                SELECT article_id FROM wikipedia_articles_systemx
                WHERE content @@@ paradedb.match(''content'', %L)
                ORDER BY paradedb.score(article_id) DESC
                LIMIT 10
            ) t',
            q
        ) INTO result_count;
    END LOOP;

    total_ms := EXTRACT(EPOCH FROM (clock_timestamp() - start_time)) * 1000;
    RAISE NOTICE 'Total time for 20 queries: % ms', round(total_ms, 1);
    RAISE NOTICE 'Average query time: % ms', round(total_ms / 20, 1);
END $$;

-- Benchmark 6: Large LIMIT queries
\echo ''
\echo '=== Benchmark 6: Large LIMIT Queries ==='
\echo 'Fetching more results per query'

\echo 'Query: "history" LIMIT 100'
EXPLAIN ANALYZE
SELECT article_id, title
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'history')
ORDER BY paradedb.score(article_id) DESC
LIMIT 100;

\echo ''
\echo 'Query: "science technology" LIMIT 100'
EXPLAIN ANALYZE
SELECT article_id, title
FROM wikipedia_articles_systemx
WHERE content @@@ paradedb.match('content', 'science technology')
ORDER BY paradedb.score(article_id) DESC
LIMIT 100;

\echo ''
\echo '=== Wikipedia Query Benchmarks Complete (System X) ==='
