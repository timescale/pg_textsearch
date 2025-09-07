-- Test file for LIMIT detection and optimization in Tapir
-- This tests Sven's concern about LIMIT clause handling

-- Disable duration logging to avoid timing differences in sanitizer tests
SET log_duration = off;

-- Load tapir extension
CREATE EXTENSION IF NOT EXISTS tapir;

-- Create test table with sufficient data for meaningful LIMIT testing
CREATE TABLE limit_test (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT
);

-- Insert test data with varied content for BM25 scoring
INSERT INTO limit_test (title, content) VALUES 
    ('Database Systems', 'postgresql database management with advanced indexing and query optimization'),
    ('Search Algorithms', 'full text search algorithms with relevance scoring and ranking mechanisms'),
    ('Information Retrieval', 'information retrieval systems using vector space models and term frequency'),
    ('Machine Learning', 'machine learning algorithms for data analysis and pattern recognition'),
    ('Data Mining', 'data mining techniques for knowledge discovery in large databases'),
    ('Text Processing', 'natural language processing and text analysis with stemming algorithms'),
    ('Database Performance', 'database optimization strategies for improving query performance'),
    ('Search Engines', 'search engine design with ranking algorithms and user experience'),
    ('Big Data Analytics', 'big data processing platforms for massive dataset analysis'),
    ('Distributed Systems', 'distributed computing and scalability in cloud environments'),
    ('Database Architecture', 'database system architecture with storage and indexing strategies'),
    ('Query Optimization', 'query optimization techniques for efficient database operations'),
    ('Text Analytics', 'text mining and document classification using statistical methods'),
    ('Information Systems', 'information management systems with data organization principles'),
    ('Database Theory', 'theoretical foundations of database systems and relational algebra'),
    ('Search Technology', 'search technology innovations with semantic understanding capabilities'),
    ('Data Structures', 'efficient data structures for database storage and retrieval operations'),
    ('Algorithm Design', 'algorithm design principles for optimal computational performance'),
    ('System Performance', 'system performance tuning and resource optimization strategies'),
    ('Database Security', 'database security measures and access control mechanisms');

-- Create tapir index
CREATE INDEX limit_test_idx ON limit_test USING tapir(content) WITH (text_config='english');

-- Test 1: Basic LIMIT functionality
-- Should detect and optimize for LIMIT 5
EXPLAIN (COSTS OFF)
SELECT title, content, content <@> to_tpvector('database', 'limit_test_idx') as score
FROM limit_test 
ORDER BY 3
LIMIT 5;

SELECT title, content, ROUND((content <@> to_tpvector('database', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
ORDER BY 3
LIMIT 5;

-- Test 2: Different LIMIT values
-- Test LIMIT 1 (should be highly optimized)
SELECT title, ROUND((content <@> to_tpvector('search', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
ORDER BY 2
LIMIT 1;

-- Test LIMIT 3 
SELECT title, ROUND((content <@> to_tpvector('optimization', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
ORDER BY 2
LIMIT 3;

-- Test LIMIT 10
SELECT title, ROUND((content <@> to_tpvector('algorithm', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
ORDER BY 2
LIMIT 10;

-- Test 3: LIMIT with WHERE clause (should prevent pushdown for safety)
-- This should NOT use LIMIT pushdown due to additional WHERE clause
EXPLAIN (COSTS OFF)
SELECT title, ROUND((content <@> to_tpvector('database system', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
WHERE id > 5
ORDER BY 2
LIMIT 7;

SELECT title, ROUND((content <@> to_tpvector('database system', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
WHERE id > 5
ORDER BY 2
LIMIT 7;

-- Test 4: No LIMIT (should use default behavior)
-- Note: Query plan varies by PG version - not testing EXPLAIN here

-- Test 5: LIMIT with OFFSET
SELECT title, ROUND((content <@> to_tpvector('performance', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
ORDER BY 2
LIMIT 5 OFFSET 2;

-- Test 6: Very small LIMIT (edge case)
SELECT title, ROUND((content <@> to_tpvector('text', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
ORDER BY 2
LIMIT 1;

-- Test 7: Large LIMIT (should still use index optimization)
SELECT COUNT(*)
FROM (
    SELECT title, content <@> to_tpvector('system', 'limit_test_idx') as score
    FROM limit_test 
    ORDER BY 2
    LIMIT 1000
) subq;

-- Test 8: LIMIT in subquery
SELECT * FROM (
    SELECT title, ROUND((content <@> to_tpvector('mining', 'limit_test_idx'))::numeric, 4) as score
    FROM limit_test 
    ORDER BY 2
    LIMIT 3
) sub WHERE score < 0;

-- Test 9: Multiple queries with different LIMIT values to test limit storage/cleanup
SELECT 'Query 1' as query_name, COUNT(*) as results FROM (
    SELECT title FROM limit_test 
    WHERE content <@> to_tpvector('database', 'limit_test_idx') < -1
    LIMIT 2
) q1;

SELECT 'Query 2' as query_name, COUNT(*) as results FROM (
    SELECT title FROM limit_test 
    WHERE content <@> to_tpvector('search', 'limit_test_idx') < -1
    LIMIT 8
) q2;

SELECT 'Query 3' as query_name, COUNT(*) as results FROM (
    SELECT title FROM limit_test 
    WHERE content <@> to_tpvector('algorithm', 'limit_test_idx') < -1
    LIMIT 4
) q3;

-- Test 10: LIMIT pushdown safety verification
-- These tests verify that LIMIT pushdown is only used when safe

-- Safe case: Simple ORDER BY with tapir score, no WHERE clause
-- This SHOULD allow LIMIT pushdown
EXPLAIN (COSTS OFF)
SELECT title, content <@> to_tpvector('simple', 'limit_test_idx') as score
FROM limit_test 
ORDER BY 2
LIMIT 3;

-- Case: Multiple ORDER BY clauses  
-- Tapir requires exactly one ORDER BY, so not using the index here is correct behavior
-- Query plan varies by PG version (Sort vs Incremental Sort) - not testing EXPLAIN

-- Unsafe case: Complex WHERE clause with additional conditions
-- This should NOT allow LIMIT pushdown due to WHERE clause
-- Query plan varies by PG version - not testing EXPLAIN

-- Test 11: Verify LIMIT pushdown behavior with debug logging
-- Enable debug logging to see pushdown decisions
SET client_min_messages = DEBUG1;

-- This should show "Safe LIMIT pushdown detected"
SELECT title, ROUND((content <@> to_tpvector('pushdown_safe', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
ORDER BY 2
LIMIT 2;

-- This should show "LIMIT detected but pushdown is unsafe" 
SELECT title, ROUND((content <@> to_tpvector('pushdown_unsafe', 'limit_test_idx'))::numeric, 4) as score
FROM limit_test 
WHERE id % 2 = 0
ORDER BY 2
LIMIT 2;

-- Reset logging level
SET client_min_messages = NOTICE;

-- Test 12: Edge cases for LIMIT pushdown
-- Very large LIMIT (should still use index optimization efficiently)
SELECT COUNT(*) FROM (
    SELECT title, content <@> to_tpvector('large_limit', 'limit_test_idx') as score
    FROM limit_test 
    ORDER BY 2
    LIMIT 50000  -- Much larger than our dataset
) subq;

-- LIMIT 0 edge case
SELECT title, content <@> to_tpvector('zero_limit', 'limit_test_idx') as score
FROM limit_test 
ORDER BY 2
LIMIT 0;

-- Cleanup
DROP TABLE limit_test CASCADE;