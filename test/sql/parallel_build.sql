-- Test case: parallel_build
-- Tests basic index build with enough data to exercise segment writing
-- Note: Actual parallel workers require 100K+ tuples and max_parallel_maintenance_workers > 0
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET enable_seqscan = off;

-- Create a table with moderate amount of data
CREATE TABLE parallel_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert data to create meaningful segments
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
FROM generate_series(1, 5000) AS i;

-- Build index (serial since < 100K tuples)
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
SELECT id, ROUND((content <@> to_bm25query('machine learning', 'parallel_test_idx'))::numeric, 4) AS score
FROM parallel_test
ORDER BY content <@> to_bm25query('machine learning', 'parallel_test_idx')
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
