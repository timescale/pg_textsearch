-- This test demonstrates top-k pg_textsearch query patterns for efficient text search

-- Load pg_textsearch extension
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Enable score logging for testing
SET pg_textsearch.log_scores = true;
SET client_min_messages = NOTICE;

-- Setup test data with realistic documents
CREATE TABLE articles (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT,
    category TEXT
);

INSERT INTO articles (title, content, category) VALUES
    ('Introduction to Databases', 'postgresql database management system with advanced indexing capabilities for fast query processing', 'technology'),
    ('Machine Learning Fundamentals', 'machine learning algorithms and artificial intelligence techniques for data analysis and prediction', 'technology'),
    ('Text Search Algorithms', 'full text search with tapir ranking and relevance scoring for information retrieval systems', 'technology'),
    ('Information Retrieval', 'information retrieval and search engines using vector space models and term frequency analysis', 'technology'),
    ('Natural Language Processing', 'natural language processing techniques including parsing stemming and semantic analysis', 'technology'),
    ('Database Optimization', 'database indexing and query optimization strategies for improving performance in large datasets', 'technology'),
    ('Document Classification', 'text mining and document classification using machine learning and statistical methods', 'technology'),
    ('Search Engine Design', 'search relevance and ranking algorithms with focus on user experience and result quality', 'technology'),
    ('Distributed Computing', 'distributed systems and scalability challenges in modern cloud computing environments', 'technology'),
    ('Data Mining Techniques', 'data mining algorithms for pattern discovery and knowledge extraction from large databases', 'technology'),
    ('Web Search Evolution', 'evolution of web search from simple keyword matching to semantic understanding', 'history'),
    ('Big Data Analytics', 'big data analytics platforms and tools for processing massive datasets efficiently', 'technology');

-- Create pg_textsearch index with text_config
CREATE INDEX articles_tapir_idx ON articles USING bm25(content) WITH (text_config='english');

-- Disable sequential scans to force index usage
SET enable_seqscan = off;

-- Test 1: Basic text similarity search with LIMIT
EXPLAIN (COSTS OFF)
SELECT title, content, content <@> 'articles_tapir_idx:database'::bm25query as score
FROM articles
ORDER BY content <@> 'articles_tapir_idx:database'::bm25query
LIMIT 5;

-- VALIDATE_BM25: table=articles query="database" index=articles_tapir_idx
SELECT title, content, ROUND((content <@> 'articles_tapir_idx:database'::bm25query)::numeric, 4) as score
FROM articles
ORDER BY content <@> 'articles_tapir_idx:database'::bm25query
LIMIT 5;

-- Test 2: Multi-term search with ranking
EXPLAIN (COSTS OFF)
SELECT title, content, content <@> 'articles_tapir_idx:machine learning'::bm25query as score
FROM articles
ORDER BY content <@> to_bm25query('machine learning', 'articles_tapir_idx')
LIMIT 3;

-- VALIDATE_BM25: table=articles query="machine learning" index=articles_tapir_idx
SELECT title, content, ROUND((content <@> 'articles_tapir_idx:machine learning'::bm25query)::numeric, 4) as score
FROM articles
ORDER BY content <@> to_bm25query('machine learning', 'articles_tapir_idx')
LIMIT 3;

-- Test 3: Category-filtered search
EXPLAIN (COSTS OFF)
SELECT title, content, content <@> 'articles_tapir_idx:search algorithms'::bm25query as score
FROM articles
WHERE category = 'technology'
ORDER BY content <@> to_bm25query('search algorithms', 'articles_tapir_idx')
LIMIT 10;

-- VALIDATE_BM25: table=articles query="search algorithms" index=articles_tapir_idx
SELECT title, content, ROUND((content <@> 'articles_tapir_idx:search algorithms'::bm25query)::numeric, 4) as score
FROM articles
WHERE category = 'technology'
ORDER BY content <@> to_bm25query('search algorithms', 'articles_tapir_idx')
LIMIT 10;

-- Test 4: Find similar articles to a specific one (standalone scoring with explicit corpus)
SELECT a2.title, a2.content, ROUND((a2.content <@> to_bm25query(a1.content, 'articles_tapir_idx'))::numeric, 4) as score
FROM articles a1, articles a2
WHERE a1.id = 3  -- "Text Search Algorithms" article
  AND a2.id != a1.id
ORDER BY a2.content <@> to_bm25query(a1.content, 'articles_tapir_idx')
LIMIT 5;

-- Test 5: Top results with scoring
EXPLAIN (COSTS OFF)
SELECT title, content, content <@> 'articles_tapir_idx:database optimization'::bm25query as score
FROM articles
ORDER BY content <@> to_bm25query('database optimization', 'articles_tapir_idx')
LIMIT 10;

-- VALIDATE_BM25: table=articles query="database optimization" index=articles_tapir_idx
SELECT title, content, ROUND((content <@> 'articles_tapir_idx:database optimization'::bm25query)::numeric, 4) as score
FROM articles
ORDER BY content <@> to_bm25query('database optimization', 'articles_tapir_idx')
LIMIT 10;

-- Test 6: Batch search with different queries
-- Note: In PG18, queries without index names fail due to eager evaluation
-- This test uses the index name to work correctly
EXPLAIN (COSTS OFF)
WITH search_terms AS (
    SELECT unnest(ARRAY['database', 'machine learning', 'search algorithms', 'text mining']) as term
)
SELECT s.term, a.title, ROUND((a.content <@> to_bm25query(s.term, 'articles_tapir_idx'))::numeric, 4) as score
FROM search_terms s
CROSS JOIN articles a
ORDER BY s.term, a.content <@> to_bm25query(s.term, 'articles_tapir_idx'), a.title;

-- Test scoring consistency for multi-term query
\echo 'Testing ORDER BY vs standalone scoring for multi-term queries'
WITH order_by_results AS (
    SELECT id, title,
           ROUND((content <@> 'articles_tapir_idx:machine learning algorithm'::bm25query)::numeric, 6) as order_by_score
    FROM articles
    ORDER BY content <@> to_bm25query('machine learning algorithm', 'articles_tapir_idx')
    LIMIT 2
),
standalone_results AS (
    SELECT id, title,
           ROUND((content <@> 'articles_tapir_idx:machine learning algorithm'::bm25query)::numeric, 6) as standalone_score
    FROM articles
    WHERE id IN (SELECT id FROM order_by_results)
)
SELECT o.id, o.title,
       o.order_by_score,
       s.standalone_score,
       CASE WHEN abs(o.order_by_score - s.standalone_score) < 0.000001
            THEN '✓ CONSISTENT'
            ELSE '✗ INCONSISTENT'
       END as consistency_check
FROM order_by_results o
JOIN standalone_results s ON o.id = s.id
ORDER BY o.id;

-- Cleanup
DROP TABLE articles CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
