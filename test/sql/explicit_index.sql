-- Test explicit index name in to_bm25query
-- Validates fixes for GitHub issues #183 and #194
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-----------------------------------------------------------------------
-- Issue #194: to_bm25query with explicit index ignores index column
-- Using an index on a different column should error
-----------------------------------------------------------------------

-- Create table with TWO text columns
CREATE TABLE explicit_index_test (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT
);

-- Index on 'content' column only
CREATE INDEX content_idx ON explicit_index_test USING bm25 (content)
    WITH (text_config = 'english');

-- Insert test data
INSERT INTO explicit_index_test (title, content) VALUES
    ('rust c java', 'postgres mysql'),
    ('python ruby', 'redis mongodb');

-- This should ERROR: using content_idx to search 'title' column
-- The index is on 'content' but we're searching 'title'
SELECT * FROM explicit_index_test
    ORDER BY title <@> to_bm25query('postgres', 'content_idx');

-- Positive test: using correct column should work
SELECT id, content <@> to_bm25query('postgres', 'content_idx') AS score
FROM explicit_index_test
ORDER BY score
LIMIT 10;

-- This should ERROR: left operand is a constant, not a column
SELECT 'hello' <@> to_bm25query('postgres', 'content_idx') AS score;

-- Clean up
DROP TABLE explicit_index_test;

-----------------------------------------------------------------------
-- Issue #183: to_bm25query with explicit index name does not force
-- planner to use that index
-----------------------------------------------------------------------

-- Create table
CREATE TABLE multi_index_test (
    id SERIAL PRIMARY KEY,
    category_id INT,
    content TEXT
);

-- Insert test data - enough rows to make planner consider index choices
INSERT INTO multi_index_test (category_id, content)
SELECT
    (i % 10),
    'Les ressources humaines gèrent les employés et leurs contrats de travail'
FROM generate_series(1, 1000) i;

-- Create two BM25 indexes with different text configurations on same column
CREATE INDEX content_idx_french ON multi_index_test USING bm25 (content)
    WITH (text_config = 'french');
CREATE INDEX content_idx_simple ON multi_index_test USING bm25 (content)
    WITH (text_config = 'simple');

-- Analyze for statistics
ANALYZE multi_index_test;

-- Query explicitly requesting the French index
-- The planner should use content_idx_french as specified
EXPLAIN (COSTS OFF)
SELECT id, content <@> to_bm25query('ressources', 'content_idx_french') AS score
FROM multi_index_test
WHERE category_id = 1
ORDER BY score
LIMIT 10;

-- Positive test: without explicit index, implicit resolution works
-- (uses first found index, with a warning about multiple indexes)
SELECT id, content <@> 'ressources' AS score
FROM multi_index_test
WHERE category_id = 1
ORDER BY score
LIMIT 3;

-- Positive test: explicit index matching the scanned index works
-- Drop the simple index so french is the only choice
DROP INDEX content_idx_simple;
EXPLAIN (COSTS OFF)
SELECT id, content <@> to_bm25query('ressources', 'content_idx_french') AS score
FROM multi_index_test
WHERE category_id = 1
ORDER BY score
LIMIT 10;

-- Clean up
DROP TABLE multi_index_test;

DROP EXTENSION pg_textsearch CASCADE;
