-- MS MARCO Passage Ranking - Query Benchmarks
-- Runs various query workloads against the indexed MS MARCO collection

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO Query Benchmarks ==='
\echo ''

-- Warm up: ensure index is in memory
\echo 'Warming up index...'
SELECT COUNT(*) FROM msmarco_passages
WHERE passage_text <@> to_bm25query('test query', 'msmarco_bm25_idx') < 0;

-- Benchmark 1: Single query latency (sample of dev queries)
\echo ''
\echo '=== Benchmark 1: Top-10 Query Latency (10 sample queries) ==='
\echo 'Running 10 queries from the dev set, returning top 10 results each'

\timing on

-- Query 1
SELECT p.passage_id,
       p.passage_text <@> to_bm25query(q.query_text, 'msmarco_bm25_idx') as score
FROM msmarco_passages p, msmarco_queries q
WHERE q.query_id = 1048585
ORDER BY score
LIMIT 10;

-- Query 2
SELECT p.passage_id,
       p.passage_text <@> to_bm25query(q.query_text, 'msmarco_bm25_idx') as score
FROM msmarco_passages p, msmarco_queries q
WHERE q.query_id = 1048586
ORDER BY score
LIMIT 10;

-- Query 3
SELECT p.passage_id,
       p.passage_text <@> to_bm25query(q.query_text, 'msmarco_bm25_idx') as score
FROM msmarco_passages p, msmarco_queries q
WHERE q.query_id = 1048587
ORDER BY score
LIMIT 10;

-- Benchmark 2: Batch query throughput
\echo ''
\echo '=== Benchmark 2: Batch Query Processing (100 queries) ==='
\echo 'Processing 100 queries, counting matching documents'

SELECT
    q.query_id,
    LEFT(q.query_text, 40) as query_preview,
    COUNT(*) FILTER (
        WHERE p.passage_text <@> to_bm25query(q.query_text, 'msmarco_bm25_idx') < 0
    ) as matching_docs
FROM msmarco_queries q
CROSS JOIN LATERAL (
    SELECT passage_id, passage_text
    FROM msmarco_passages
    LIMIT 10000  -- Sample for throughput test
) p
WHERE q.query_id IN (
    SELECT query_id FROM msmarco_queries ORDER BY query_id LIMIT 100
)
GROUP BY q.query_id, q.query_text
ORDER BY q.query_id
LIMIT 20;

-- Benchmark 3: Relevance evaluation (MRR@10)
\echo ''
\echo '=== Benchmark 3: Search Quality - MRR@10 ==='
\echo 'Computing Mean Reciprocal Rank for queries with known relevant passages'

WITH ranked_results AS (
    SELECT
        q.query_id,
        p.passage_id,
        p.passage_text <@> to_bm25query(q.query_text, 'msmarco_bm25_idx') as score,
        ROW_NUMBER() OVER (
            PARTITION BY q.query_id
            ORDER BY p.passage_text <@> to_bm25query(q.query_text, 'msmarco_bm25_idx')
        ) as rank
    FROM msmarco_queries q
    CROSS JOIN msmarco_passages p
    WHERE q.query_id IN (SELECT query_id FROM msmarco_qrels LIMIT 100)
),
top10_results AS (
    SELECT * FROM ranked_results WHERE rank <= 10
),
reciprocal_ranks AS (
    SELECT
        t.query_id,
        MIN(CASE WHEN qr.passage_id IS NOT NULL THEN 1.0 / t.rank END) as rr
    FROM top10_results t
    LEFT JOIN msmarco_qrels qr
        ON t.query_id = qr.query_id AND t.passage_id = qr.passage_id
    GROUP BY t.query_id
)
SELECT
    'MRR@10' as metric,
    ROUND(AVG(COALESCE(rr, 0))::numeric, 4) as value,
    COUNT(*) as num_queries
FROM reciprocal_ranks;

-- Benchmark 4: Common query patterns
\echo ''
\echo '=== Benchmark 4: Common Query Patterns ==='

\echo 'Short query (1-2 words):'
EXPLAIN ANALYZE
SELECT passage_id, passage_text <@> to_bm25query('coffee', 'msmarco_bm25_idx') as score
FROM msmarco_passages
ORDER BY score
LIMIT 10;

\echo ''
\echo 'Medium query (3-5 words):'
EXPLAIN ANALYZE
SELECT passage_id, passage_text <@> to_bm25query('how to make coffee', 'msmarco_bm25_idx') as score
FROM msmarco_passages
ORDER BY score
LIMIT 10;

\echo ''
\echo 'Long query (question):'
EXPLAIN ANALYZE
SELECT passage_id,
       passage_text <@> to_bm25query(
           'what is the best way to brew coffee at home',
           'msmarco_bm25_idx'
       ) as score
FROM msmarco_passages
ORDER BY score
LIMIT 10;

\echo ''
\echo '=== MS MARCO Query Benchmarks Complete ==='
