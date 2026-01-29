-- Test case: parallel_build
-- Tests parallel index build with enough data to trigger parallel workers
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET enable_seqscan = off;

-- Force parallel build settings
SET max_parallel_maintenance_workers = 2;
SET maintenance_work_mem = '256MB';
SET pg_textsearch.parallel_build_expansion_factor = 10.0;

-- Create a table with enough data to trigger parallel build (100K+ tuples)
CREATE TABLE parallel_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert 150K rows to exceed the 100K threshold for parallel build
INSERT INTO parallel_test (content)
SELECT 'Document number ' || i || ' contains words like ' ||
       CASE i % 5
           WHEN 0 THEN 'apple banana cherry'
           WHEN 1 THEN 'database query optimization'
           WHEN 2 THEN 'machine learning algorithm'
           WHEN 3 THEN 'distributed systems design'
           ELSE 'network protocol security'
       END ||
       ' and additional text to make it longer ' ||
       CASE i % 3
           WHEN 0 THEN 'with some extra content here'
           WHEN 1 THEN 'including more varied terms'
           ELSE 'plus different phrases altogether'
       END
FROM generate_series(1, 150000) AS i;

-- Update statistics so planner knows table size
ANALYZE parallel_test;

-- Build index (should use parallel workers with 150K tuples)
CREATE INDEX parallel_test_idx ON parallel_test USING bm25(content)
  WITH (text_config='english');

-- Verify index was created
SELECT 'Index created' AS status
FROM pg_class
WHERE relname = 'parallel_test_idx';

-- Verify queries work correctly
SELECT COUNT(*) AS apple_count
FROM parallel_test
WHERE content <@> to_bm25query('apple', 'parallel_test_idx') < 0;

SELECT COUNT(*) AS database_count
FROM parallel_test
WHERE content <@> to_bm25query('database', 'parallel_test_idx') < 0;

-- Verify we can get top-k results with correct ordering
-- Limit to first 100 IDs for deterministic results (many docs have identical scores)
SELECT id, ROUND((content <@> to_bm25query('machine learning', 'parallel_test_idx'))::numeric, 4) AS score
FROM parallel_test
WHERE id <= 100
ORDER BY content <@> to_bm25query('machine learning', 'parallel_test_idx'), id
LIMIT 5;

-- Verify index statistics look reasonable
SELECT bm25_summarize_index('parallel_test_idx') IS NOT NULL AS has_summary;

-- Test that we can do additional inserts after build
INSERT INTO parallel_test (content) VALUES ('new document after build with unique terms');

-- Verify the new document is searchable
SELECT COUNT(*) AS new_doc_found
FROM parallel_test
WHERE content <@> to_bm25query('unique', 'parallel_test_idx') < 0;

-- Cleanup
DROP TABLE parallel_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
