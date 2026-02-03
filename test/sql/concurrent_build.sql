-- Test CREATE INDEX CONCURRENTLY with bm25 indexes
--
-- NOTE: CREATE INDEX CONCURRENTLY is currently accepted syntactically but
-- has a known bug where documents may be indexed twice during the validation
-- phase. This causes duplicate results in queries. A proper fix requires
-- implementing the ambulkdelete callback to correctly report existing TIDs
-- to the validate_index phase.
--
-- This test verifies the current behavior: CIC completes successfully and
-- the index is marked valid, even though it may have duplicate entries.

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
  WITH (text_config='english');

-- Verify index is valid (indisvalid = true)
SELECT indisvalid FROM pg_index WHERE indexrelid = 'cic_basic_idx'::regclass;

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
  WITH (text_config='english');

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
