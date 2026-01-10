-- Compression validation test
-- Verifies that compressed and uncompressed indexes produce identical results
-- This is a critical correctness test, not a performance test

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test corpus with varied content
CREATE TABLE validation_docs (
    id serial PRIMARY KEY,
    content text
);

-- Insert documents with varying term frequencies and lengths
INSERT INTO validation_docs (content) VALUES
    ('the quick brown fox jumps over the lazy dog'),
    ('the quick brown fox'),
    ('fox fox fox fox fox'),  -- high TF
    ('the'),  -- single term
    ('a very long document with many words to test length normalization in bm25 scoring algorithm implementation'),
    ('search engine optimization techniques for better ranking'),
    ('machine learning and artificial intelligence'),
    ('database query optimization strategies'),
    ('full text search with bm25 ranking'),
    ('postgres extension development guide');

-- Add more documents to ensure multiple blocks
INSERT INTO validation_docs (content)
SELECT 'document number ' || i || ' with term fox and search'
FROM generate_series(1, 200) AS i;

-- =============================================================================
-- Phase 1: Build compressed index and capture results
-- =============================================================================

SET pg_textsearch.compress_segments = on;

CREATE INDEX validation_compressed_idx ON validation_docs
    USING bm25 (content) WITH (text_config = 'english');

SELECT bm25_spill_index('validation_compressed_idx');

-- Capture results for various queries
CREATE TEMP TABLE compressed_results AS
SELECT
    'fox' as query,
    id,
    content <@> to_bm25query('fox', 'validation_compressed_idx') as score
FROM validation_docs
WHERE content <@> to_bm25query('fox', 'validation_compressed_idx') < 0
ORDER BY score, id
LIMIT 50;

INSERT INTO compressed_results
SELECT
    'quick brown' as query,
    id,
    content <@> to_bm25query('quick brown', 'validation_compressed_idx') as score
FROM validation_docs
WHERE content <@> to_bm25query('quick brown', 'validation_compressed_idx') < 0
ORDER BY score, id
LIMIT 50;

INSERT INTO compressed_results
SELECT
    'search engine' as query,
    id,
    content <@> to_bm25query('search engine', 'validation_compressed_idx') as score
FROM validation_docs
WHERE content <@> to_bm25query('search engine', 'validation_compressed_idx') < 0
ORDER BY score, id
LIMIT 50;

INSERT INTO compressed_results
SELECT
    'document number fox search' as query,
    id,
    content <@> to_bm25query('document number fox search', 'validation_compressed_idx') as score
FROM validation_docs
WHERE content <@> to_bm25query('document number fox search', 'validation_compressed_idx') < 0
ORDER BY score, id
LIMIT 50;

DROP INDEX validation_compressed_idx;

-- =============================================================================
-- Phase 2: Build uncompressed index and capture results
-- =============================================================================

SET pg_textsearch.compress_segments = off;

CREATE INDEX validation_uncompressed_idx ON validation_docs
    USING bm25 (content) WITH (text_config = 'english');

SELECT bm25_spill_index('validation_uncompressed_idx');

CREATE TEMP TABLE uncompressed_results AS
SELECT
    'fox' as query,
    id,
    content <@> to_bm25query('fox', 'validation_uncompressed_idx') as score
FROM validation_docs
WHERE content <@> to_bm25query('fox', 'validation_uncompressed_idx') < 0
ORDER BY score, id
LIMIT 50;

INSERT INTO uncompressed_results
SELECT
    'quick brown' as query,
    id,
    content <@> to_bm25query('quick brown', 'validation_uncompressed_idx') as score
FROM validation_docs
WHERE content <@> to_bm25query('quick brown', 'validation_uncompressed_idx') < 0
ORDER BY score, id
LIMIT 50;

INSERT INTO uncompressed_results
SELECT
    'search engine' as query,
    id,
    content <@> to_bm25query('search engine', 'validation_uncompressed_idx') as score
FROM validation_docs
WHERE content <@> to_bm25query('search engine', 'validation_uncompressed_idx') < 0
ORDER BY score, id
LIMIT 50;

INSERT INTO uncompressed_results
SELECT
    'document number fox search' as query,
    id,
    content <@> to_bm25query('document number fox search', 'validation_uncompressed_idx') as score
FROM validation_docs
WHERE content <@> to_bm25query('document number fox search', 'validation_uncompressed_idx') < 0
ORDER BY score, id
LIMIT 50;

-- =============================================================================
-- Phase 3: Compare results
-- =============================================================================

-- Check for any differences in ranking or scores
SELECT 'Checking for ranking differences...' as status;

-- This should return 0 rows if compression is working correctly
SELECT
    c.query,
    c.id as compressed_id,
    u.id as uncompressed_id,
    c.score as compressed_score,
    u.score as uncompressed_score,
    abs(c.score - u.score) as score_diff
FROM compressed_results c
FULL OUTER JOIN uncompressed_results u
    ON c.query = u.query AND c.id = u.id
WHERE c.id IS NULL
   OR u.id IS NULL
   OR abs(c.score - u.score) > 0.0001;  -- Allow tiny floating point differences

-- Summary statistics
SELECT
    'Total compressed results' as metric,
    count(*) as value
FROM compressed_results
UNION ALL
SELECT
    'Total uncompressed results',
    count(*)
FROM uncompressed_results
UNION ALL
SELECT
    'Matching results',
    count(*)
FROM compressed_results c
JOIN uncompressed_results u ON c.query = u.query AND c.id = u.id
WHERE abs(c.score - u.score) < 0.0001;

-- Verify counts match exactly
SELECT
    CASE
        WHEN (SELECT count(*) FROM compressed_results) =
             (SELECT count(*) FROM uncompressed_results)
        THEN 'PASS: Result counts match'
        ELSE 'FAIL: Result counts differ'
    END as validation_result;

-- Clean up
DROP TABLE validation_docs CASCADE;
