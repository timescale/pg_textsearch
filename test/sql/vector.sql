-- Test bm25vector type and operators functionality

-- Load pg_textsearch extension
CREATE EXTENSION IF NOT EXISTS pg_textsearch;


-- Enable score logging for testing
SET pg_textsearch.log_scores = true;
SET client_min_messages = NOTICE;
SET enable_seqscan = false;

-- Cleanup any existing state first
DROP TABLE IF EXISTS test_docs CASCADE;

-- Setup test table
CREATE TABLE test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO test_docs (content) VALUES
    ('the quick brown fox'),
    ('jumped over the lazy dog'),
    ('sphinx of black quartz'),
    ('hello world example'),
    ('postgresql full text search');

-- Create pg_textsearch index
CREATE INDEX docs_vector_idx ON test_docs USING bm25(content) WITH (text_config='english');

-- Test bm25vector I/O functions
-- Test input/output with index name format
SELECT 'docs_vector_idx:{hello:2,world:1}'::bm25vector;
SELECT 'docs_vector_idx:{}'::bm25vector;

-- Test pg_textsearch scoring using text <@> bm25query operations
SELECT
    content,
    ROUND((content <@> to_bm25query('hello world', 'docs_vector_idx'))::numeric, 4) as score
FROM test_docs
ORDER BY content <@> to_bm25query('hello world', 'docs_vector_idx')
LIMIT 10;

-- Test different query terms with multi-term query
SELECT
    content,
    ROUND((content <@> to_bm25query('postgresql search', 'docs_vector_idx'))::numeric, 4) as score
FROM test_docs
ORDER BY content <@> to_bm25query('postgresql search', 'docs_vector_idx')
LIMIT 10;

-- Test bm25vector equality operator
SELECT 'docs_vector_idx:{hello:1,world:2}'::bm25vector = 'docs_vector_idx:{hello:1,world:2}'::bm25vector;
SELECT 'docs_vector_idx:{hello:1,world:2}'::bm25vector = 'docs_vector_idx:{world:2,hello:1}'::bm25vector;

-- Test scoring with real documents
SELECT

    id,
    content,
    ROUND((content <@> to_bm25query('quick fox', 'docs_vector_idx'))::numeric, 4) as relevance
FROM test_docs
ORDER BY content <@> to_bm25query('quick fox', 'docs_vector_idx')  -- BM25 scoring for ranking
LIMIT 3;

-- Test with another index using simple config
CREATE INDEX docs_simple_idx ON test_docs USING bm25(content) WITH (text_config='simple');

-- Test scoring consistency: ORDER BY vs standalone scoring
\echo 'Testing ORDER BY vs standalone scoring consistency'
WITH order_by_scores AS (

    SELECT id, content,
           ROUND((content <@> to_bm25query('fox dog', 'docs_simple_idx'))::numeric, 6) as order_by_score
    FROM test_docs
    ORDER BY content <@> to_bm25query('fox dog', 'docs_simple_idx')
    LIMIT 3
),
standalone_scores AS (
    -- Force standalone scoring by using all documents and calculating scores explicitly
    SELECT id, content,
           ROUND((content <@> to_bm25query('fox dog', 'docs_simple_idx'))::numeric, 6) as standalone_score
    FROM test_docs
    -- No WHERE clause to force sequential scan and standalone scoring for all rows
)
SELECT o.id,
       o.order_by_score,
       s.standalone_score,
       CASE WHEN abs(o.order_by_score - s.standalone_score) < 0.000001
            THEN '✓ SCORES MATCH'
            ELSE '✗ SCORES DIFFER'
       END as score_consistency
FROM order_by_scores o
JOIN standalone_scores s ON o.id = s.id
ORDER BY o.id;

-- Cleanup
DROP INDEX docs_vector_idx;
DROP INDEX docs_simple_idx;
DROP TABLE test_docs;
DROP EXTENSION pg_textsearch CASCADE;
