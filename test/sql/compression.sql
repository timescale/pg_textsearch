-- Test compression functionality
-- Tests that compression (now default) produces correct results

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Compression is now on by default
SHOW pg_textsearch.compress_segments;

-- Create test table
CREATE TABLE compression_docs (
    id serial PRIMARY KEY,
    content text
);

-- Create BM25 index
CREATE INDEX compression_idx ON compression_docs
    USING bm25 (content) WITH (text_config = 'english');

-- Insert enough documents to create multiple blocks (block size is 128)
-- We need at least 128+ docs with the same term to test block compression
INSERT INTO compression_docs (content)
SELECT 'the quick brown fox jumps over lazy dog number ' || i
FROM generate_series(1, 200) AS i;

-- Force segment creation by spilling memtable
SELECT bm25_spill_index('compression_idx');

-- Query should work correctly with compressed segments
SELECT count(*) FROM compression_docs
WHERE content <@> to_bm25query('fox', 'compression_idx') < 0;

-- Test with multiple query terms
SELECT count(*) FROM compression_docs
WHERE content <@> to_bm25query('quick brown', 'compression_idx') < 0;

-- Test that scores are reasonable (not corrupted by compression)
-- Scores should be negative (BM25 returns negative for ORDER BY ASC)
SELECT id, content <@> to_bm25query('fox', 'compression_idx') AS score
FROM compression_docs
WHERE content <@> to_bm25query('fox', 'compression_idx') < 0
ORDER BY score
LIMIT 5;

-- Add more documents and test again
INSERT INTO compression_docs (content)
SELECT 'different content with unique words like zephyr ' || i
FROM generate_series(1, 100) AS i;

-- Spill again to create a second segment
SELECT bm25_spill_index('compression_idx');

-- Query across both segments
SELECT count(*) FROM compression_docs
WHERE content <@> to_bm25query('fox', 'compression_idx') < 0;

-- Query the new content
SELECT count(*) FROM compression_docs
WHERE content <@> to_bm25query('zephyr', 'compression_idx') < 0;

-- Test with BMW optimization enabled (default)
SET pg_textsearch.enable_bmw = on;
SELECT id FROM compression_docs
WHERE content <@> to_bm25query('fox', 'compression_idx') < 0
ORDER BY content <@> to_bm25query('fox', 'compression_idx')
LIMIT 5;

-- Test with BMW disabled to exercise different code path
SET pg_textsearch.enable_bmw = off;
SELECT id FROM compression_docs
WHERE content <@> to_bm25query('fox', 'compression_idx') < 0
ORDER BY content <@> to_bm25query('fox', 'compression_idx')
LIMIT 5;

-- Reset settings
SET pg_textsearch.enable_bmw = on;

-- Clean up
DROP TABLE compression_docs;

-- Test that compression can be disabled
SET pg_textsearch.compress_segments = off;
SHOW pg_textsearch.compress_segments;

-- Create another table with compression off
CREATE TABLE uncompressed_docs (
    id serial PRIMARY KEY,
    content text
);

CREATE INDEX uncompressed_idx ON uncompressed_docs
    USING bm25 (content) WITH (text_config = 'english');

INSERT INTO uncompressed_docs (content)
SELECT 'testing uncompressed segments ' || i
FROM generate_series(1, 150) AS i;

SELECT bm25_spill_index('uncompressed_idx');

-- Should work the same
SELECT count(*) FROM uncompressed_docs
WHERE content <@> to_bm25query('testing', 'uncompressed_idx') < 0;

-- Clean up
DROP TABLE uncompressed_docs;
