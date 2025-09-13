-- This test demonstrates top-k tapir query patterns for efficient text search

-- Load tapir extension
CREATE EXTENSION IF NOT EXISTS tapir;

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

-- Create tapir index with text_config
CREATE INDEX articles_tapir_idx ON articles USING tapir(content) WITH (text_config='english');

-- Disable sequential scans to force index usage
SET enable_seqscan = off;

-- Test 1: Basic text similarity search with LIMIT
EXPLAIN (COSTS OFF) 
SELECT title, content, content <@> to_tpvector('database', 'articles_tapir_idx') as score
FROM articles 
ORDER BY 3
LIMIT 5;

SELECT title, content, ROUND((content <@> to_tpvector('database', 'articles_tapir_idx'))::numeric, 4) as score
FROM articles 
ORDER BY 3
LIMIT 5;

-- Test 2: Multi-term search with ranking
EXPLAIN (COSTS OFF)
SELECT title, content, content <@> to_tpvector('machine learning', 'articles_tapir_idx') as score
FROM articles 
ORDER BY 3
LIMIT 3;

SELECT title, content, ROUND((content <@> to_tpvector('machine learning', 'articles_tapir_idx'))::numeric, 4) as score
FROM articles 
ORDER BY 3
LIMIT 3;

-- Test 3: Category-filtered search
EXPLAIN (COSTS OFF)
SELECT title, content, content <@> to_tpvector('search algorithms', 'articles_tapir_idx') as score
FROM articles 
WHERE category = 'technology'
ORDER BY 3
LIMIT 10;

SELECT title, content, ROUND((content <@> to_tpvector('search algorithms', 'articles_tapir_idx'))::numeric, 4) as score
FROM articles 
WHERE category = 'technology'
ORDER BY 3
LIMIT 10;

-- Test 4: Find similar articles to a specific one (no index)
SELECT a2.title, a2.content, ROUND((a2.content <@> to_tpvector(a1.content, 'articles_tapir_idx'))::numeric, 4) as score
FROM articles a1, articles a2
WHERE a1.id = 3  -- "Text Search Algorithms" article
  AND a2.id != a1.id
ORDER BY 3
LIMIT 5;

-- Test 5: Top results with scoring
EXPLAIN (COSTS OFF)
SELECT title, content, content <@> to_tpvector('database optimization', 'articles_tapir_idx') as score
FROM articles 
ORDER BY 3
LIMIT 10;

SELECT title, content, ROUND((content <@> to_tpvector('database optimization', 'articles_tapir_idx'))::numeric, 4) as score
FROM articles 
ORDER BY 3
LIMIT 10;

-- Test 6: Batch search with different queries (no index)
EXPLAIN (COSTS OFF)
WITH search_terms AS (
    SELECT unnest(ARRAY['database', 'machine learning', 'search algorithms', 'text mining']) as term
)
SELECT s.term, a.title, a.content <@> to_tpvector(s.term, 'articles_tapir_idx') as score
FROM search_terms s
CROSS JOIN articles a
ORDER BY s.term, a.content <@> to_tpvector(s.term, 'articles_tapir_idx');

WITH search_terms AS (
    SELECT unnest(ARRAY['database', 'machine learning', 'search algorithms', 'text mining']) as term
)
SELECT s.term, a.title, ROUND((a.content <@> to_tpvector(s.term, 'articles_tapir_idx'))::numeric, 4) as score
FROM search_terms s
CROSS JOIN articles a
ORDER BY s.term, a.content <@> to_tpvector(s.term, 'articles_tapir_idx');

-- Cleanup
DROP TABLE articles CASCADE;