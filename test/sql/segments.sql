--
-- Test segment flush functionality
--

-- Set up extension
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create a test table
CREATE TABLE segment_test_docs (
    id INT PRIMARY KEY,
    doc TEXT
);

-- Set a very low memory limit to trigger flush (1MB)
SET pg_textsearch.index_memory_limit = 1;

-- Don't use DEBUG messages as they contain non-deterministic addresses
SET client_min_messages = NOTICE;

-- Create BM25 index
CREATE INDEX segment_test_idx ON segment_test_docs
USING bm25 (doc) WITH (text_config='english');

-- Insert documents in batches to trigger flush
-- Generate documents with unique terms to increase memory usage
BEGIN;

-- First batch - should fit in memory
INSERT INTO segment_test_docs
SELECT i,
       'Document ' || i || ' contains term' || i || ' and uniqueword' || i ||
       ' plus commonword test search index memory segment flush'
FROM generate_series(1, 100) i;

-- Second batch - should trigger flush
INSERT INTO segment_test_docs
SELECT i,
       'Document ' || i || ' has different term' || i || ' and specialword' || i ||
       ' with commonword test search index memory segment flush'
FROM generate_series(101, 200) i;

-- Third batch - may trigger another flush
INSERT INTO segment_test_docs
SELECT i,
       'Document ' || i || ' includes another term' || i || ' and extraword' || i ||
       ' using commonword test search index memory segment flush'
FROM generate_series(201, 300) i;

COMMIT;

-- Reset client messages
SET client_min_messages = NOTICE;

-- Test BM25 scoring still works after flush
SELECT id, doc <@> to_bm25query('segment_test_idx', 'commonword test') AS score
FROM segment_test_docs
ORDER BY score
LIMIT 5;

-- Test scoring with unique terms
SELECT id, doc <@> to_bm25query('segment_test_idx', 'term100') AS score
FROM segment_test_docs
ORDER BY score
LIMIT 3;

-- Test that we can query across all documents
SELECT COUNT(DISTINCT id) AS total_docs,
       MIN(doc <@> to_bm25query('segment_test_idx', 'document')) AS min_score,
       MAX(doc <@> to_bm25query('segment_test_idx', 'document')) AS max_score
FROM segment_test_docs;

-- Clean up
DROP TABLE segment_test_docs;
RESET pg_textsearch.index_memory_limit;
