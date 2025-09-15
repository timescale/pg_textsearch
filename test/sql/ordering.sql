-- Test ordering consistency between different ways of using index-based BM25 scoring
-- This verifies that ORDER BY scoring produces consistent results with direct score calculations

CREATE EXTENSION tapir;

-- Create test table with varied content for comprehensive scoring tests
CREATE TABLE ordering_test_docs (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT
);

-- Insert test documents with different term frequencies and document lengths
INSERT INTO ordering_test_docs (title, content) VALUES
('Database Fundamentals', 'database systems provide structured data storage and retrieval capabilities'),
('Advanced Database Design', 'database design principles focus on normalization and query optimization for database performance'),
('Search Engine Architecture', 'search engines use inverted indexes to quickly find relevant documents matching user queries'),
('Full-Text Search Systems', 'full text search systems enable users to search through large document collections efficiently'),
('Information Retrieval', 'information retrieval systems rank documents by relevance using algorithms like BM25'),
('PostgreSQL Extensions', 'postgresql extensions add functionality to the database system including search capabilities'),
('Database Optimization', 'database query optimization improves performance through better execution plans and indexes'),
('Search Algorithms', 'search algorithms including BM25 and TF-IDF calculate document relevance scores for ranking'),
('Text Processing', 'text processing involves tokenization stemming and scoring for search applications'),
('Distributed Systems', 'distributed database systems provide scalability and fault tolerance for large applications');

-- Create Tapir index for testing
CREATE INDEX ordering_test_idx ON ordering_test_docs USING tapir(content)
    WITH (text_config='english', k1=1.2, b=0.75);

-- Test 1: Compare ORDER BY ranking with explicit score calculation for single term
\echo 'Test 1: ORDER BY vs explicit score calculation - "database"'

-- Get results using ORDER BY (should use index for efficient ordering)
WITH order_by_results AS (
    SELECT id, title, ROW_NUMBER() OVER () as order_by_rank
    FROM ordering_test_docs
    ORDER BY content <@> to_tpquery('database', 'ordering_test_idx')
    LIMIT 5
),
-- Get same results by calculating scores explicitly and ordering by them
explicit_score_results AS (
    SELECT id, title,
           ROUND((content <@> to_tpquery('database', 'ordering_test_idx'))::numeric, 6) as explicit_score,
           ROW_NUMBER() OVER (ORDER BY content <@> to_tpquery('database', 'ordering_test_idx')) as explicit_rank
    FROM ordering_test_docs
    WHERE id IN (SELECT id FROM order_by_results)
)
-- Compare the rankings
SELECT o.id, o.title,
       o.order_by_rank,
       e.explicit_rank,
       e.explicit_score,
       CASE WHEN o.order_by_rank = e.explicit_rank
            THEN '✓ SAME RANK'
            ELSE '✗ RANK DIFFERS'
       END as rank_consistency
FROM order_by_results o
JOIN explicit_score_results e ON o.id = e.id
ORDER BY o.order_by_rank;

-- Test 2: Verify that ORDER BY without LIMIT gives same ranking as with LIMIT
\echo 'Test 2: ORDER BY consistency with and without LIMIT - "search"'

WITH unlimited_results AS (
    SELECT id, title,
           ROUND((content <@> to_tpquery('search', 'ordering_test_idx'))::numeric, 6) as score,
           ROW_NUMBER() OVER (ORDER BY content <@> to_tpquery('search', 'ordering_test_idx')) as unlimited_rank
    FROM ordering_test_docs
),
limited_results AS (
    SELECT id, title,
           ROUND((content <@> to_tpquery('search', 'ordering_test_idx'))::numeric, 6) as score,
           ROW_NUMBER() OVER () as limited_rank
    FROM ordering_test_docs
    ORDER BY content <@> to_tpquery('search', 'ordering_test_idx')
    LIMIT 10
)
SELECT l.id, l.title,
       l.limited_rank,
       u.unlimited_rank,
       l.score,
       CASE WHEN l.limited_rank = u.unlimited_rank
            THEN '✓ CONSISTENT'
            ELSE '✗ INCONSISTENT'
       END as rank_consistency
FROM limited_results l
JOIN unlimited_results u ON l.id = u.id
ORDER BY l.limited_rank;

-- Test 3: Test score calculation consistency across multiple queries
\echo 'Test 3: Score calculation consistency - multiple terms'

SELECT id, title,
       ROUND((content <@> to_tpquery('database', 'ordering_test_idx'))::numeric, 4) as database_score,
       ROUND((content <@> to_tpquery('search', 'ordering_test_idx'))::numeric, 4) as search_score,
       ROUND((content <@> to_tpquery('system', 'ordering_test_idx'))::numeric, 4) as system_score,
       -- Verify that multi-term queries work correctly
       ROUND((content <@> to_tpquery('database search', 'ordering_test_idx'))::numeric, 4) as combined_score
FROM ordering_test_docs
ORDER BY content <@> to_tpquery('database search', 'ordering_test_idx')
LIMIT 6;

-- Test 4: Verify zero scores are handled correctly
\echo 'Test 4: Zero score handling - non-matching terms'

SELECT id, title,
       ROUND((content <@> to_tpquery('nonexistent', 'ordering_test_idx'))::numeric, 6) as nonexistent_score,
       CASE WHEN content <@> to_tpquery('nonexistent', 'ordering_test_idx') = 0.0
            THEN '✓ ZERO SCORE'
            ELSE '✗ NON-ZERO'
       END as zero_check
FROM ordering_test_docs
ORDER BY content <@> to_tpquery('nonexistent', 'ordering_test_idx')
LIMIT 3;

-- Test 5: Verify ordering stability for identical scores
\echo 'Test 5: Ordering stability for documents with identical scores'

-- All documents should have zero score for non-matching terms
SELECT id, title,
       ROUND((content <@> to_tpquery('xyz123', 'ordering_test_idx'))::numeric, 6) as score,
       -- Should be ordered by some deterministic criteria when scores are equal
       CASE WHEN ROUND((content <@> to_tpquery('xyz123', 'ordering_test_idx'))::numeric, 6) = 0.0
            THEN '✓ ZERO'
            ELSE '✗ NON-ZERO'
       END as score_check
FROM ordering_test_docs
ORDER BY content <@> to_tpquery('xyz123', 'ordering_test_idx'), id
LIMIT 5;

-- Test 6: Complex multi-term query ordering
\echo 'Test 6: Complex query ordering consistency'

-- Test ordering with complex query
WITH complex_results AS (
    SELECT id, title,
           ROUND((content <@> to_tpquery('database system optimization', 'ordering_test_idx'))::numeric, 6) as score
    FROM ordering_test_docs
    ORDER BY content <@> to_tpquery('database system optimization', 'ordering_test_idx')
)
SELECT id, title, score,
       -- Verify scores are in ascending order (better scores first)
       CASE WHEN score <= COALESCE(LAG(score) OVER (ORDER BY score), score)
            THEN '✓ PROPER ORDER'
            ELSE '✗ WRONG ORDER'
       END as order_check
FROM complex_results;

-- Test 7: Verify that index-based ordering is actually using the index
\echo 'Test 7: Index usage verification'

-- This should be fast and use index-based ordering
EXPLAIN (COSTS OFF, BUFFERS OFF)
SELECT id, title
FROM ordering_test_docs
ORDER BY content <@> to_tpquery('database search performance', 'ordering_test_idx')
LIMIT 5;

-- Test 8: Performance comparison hint
\echo 'Test 8: Performance characteristics verification'

-- Time a simple ORDER BY query (should be fast with index)
SELECT COUNT(*) as total_docs,
       COUNT(CASE WHEN content <@> to_tpquery('database', 'ordering_test_idx') < 0 THEN 1 END) as matching_docs,
       MIN(ROUND((content <@> to_tpquery('database', 'ordering_test_idx'))::numeric, 4)) as best_score,
       MAX(ROUND((content <@> to_tpquery('database', 'ordering_test_idx'))::numeric, 4)) as worst_score
FROM ordering_test_docs;

-- Summary verification
\echo 'Summary: All tests verify that ORDER BY with Tapir index produces consistent, expected results'

SELECT
    'Index-based ORDER BY provides:' as feature,
    '1. Consistent ranking across different query patterns' as benefit_1,
    '2. Proper BM25 score calculation with corpus statistics' as benefit_2,
    '3. Efficient ordering using index structures' as benefit_3;

-- Clean up
DROP TABLE ordering_test_docs CASCADE;
