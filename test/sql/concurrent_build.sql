-- Test CREATE INDEX CONCURRENTLY with bm25 indexes
--
-- CIC requires proper ambulkdelete callback implementation to report all
-- indexed TIDs during validation phase. This ensures the validate_index
-- function can determine which tuples are already indexed and avoid
-- duplicating entries.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET enable_seqscan = off;

--------------------------------------------------------------------------------
-- Test 1: Basic CREATE INDEX CONCURRENTLY
--------------------------------------------------------------------------------
CREATE TABLE cic_basic (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO cic_basic (content) VALUES
    ('the quick brown fox jumps over the lazy dog'),
    ('postgresql is a powerful database system'),
    ('full text search with bm25 ranking');

-- Create index concurrently - should complete without error
CREATE INDEX CONCURRENTLY cic_basic_idx ON cic_basic USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Verify index is valid (indisvalid = true)
SELECT indisvalid FROM pg_index WHERE indexrelid = 'cic_basic_idx'::regclass;

-- Verify no duplicate entries (this was a bug symptom before the fix)
SELECT COUNT(*) = 1 AS no_duplicates FROM cic_basic
WHERE content <@> to_bm25query('database', 'cic_basic_idx') < 0;

-- Verify index is used in query plan
EXPLAIN (COSTS OFF) SELECT id FROM cic_basic
WHERE content <@> to_bm25query('database', 'cic_basic_idx') < 0
ORDER BY content <@> to_bm25query('database', 'cic_basic_idx')
LIMIT 3;

--------------------------------------------------------------------------------
-- Test 2: DROP INDEX CONCURRENTLY
--------------------------------------------------------------------------------
DROP INDEX CONCURRENTLY cic_basic_idx;

-- Verify index is gone
SELECT COUNT(*) FROM pg_indexes WHERE indexname = 'cic_basic_idx';

--------------------------------------------------------------------------------
-- Test 3: Recreate with regular CREATE INDEX for comparison
--------------------------------------------------------------------------------
CREATE INDEX cic_basic_idx ON cic_basic USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Verify index is valid
SELECT indisvalid FROM pg_index WHERE indexrelid = 'cic_basic_idx'::regclass;

-- Query should work and return expected results
SELECT id FROM cic_basic
WHERE content <@> to_bm25query('database', 'cic_basic_idx') < 0
ORDER BY content <@> to_bm25query('database', 'cic_basic_idx');

--------------------------------------------------------------------------------
-- Cleanup
--------------------------------------------------------------------------------
DROP TABLE cic_basic CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
