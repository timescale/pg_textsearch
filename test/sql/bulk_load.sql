-- Test bulk load spill threshold
-- Exercises state.c bulk_load_spill_check path

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Set a very low bulk load threshold to trigger automatic spill
SET pg_textsearch.bulk_load_threshold = 100;

-- Create table and index
CREATE TABLE bulk_load_test (id serial PRIMARY KEY, content text);
CREATE INDEX bulk_load_idx ON bulk_load_test USING bm25(content)
    WITH (text_config='english');

-- Insert enough distinct terms in one transaction to exceed threshold
-- With threshold=100, this should trigger auto-spill at commit
BEGIN;
INSERT INTO bulk_load_test (content)
SELECT 'uniqueterm' || i || ' extra words for length variation'
FROM generate_series(1, 200) AS i;
COMMIT;

-- Verify the data is searchable (spill should have happened)
SELECT count(*) FROM bulk_load_test
WHERE content <@> to_bm25query('uniqueterm1', 'bulk_load_idx') < 0;

-- Check index summary shows segment data
SELECT bm25_summarize_index('bulk_load_idx') IS NOT NULL AS has_summary;

-- =============================================================================
-- Test 2: Double bulk load spill with pre-existing L0 segment
-- Exercises state.c segment chain linking in tp_bulk_load_spill_check
-- =============================================================================

-- First, create a segment by spilling the existing memtable
SELECT bm25_spill_index('bulk_load_idx');

-- Now insert more data in a single transaction with low threshold
-- This triggers bulk_load_spill_check with a pre-existing L0 chain
BEGIN;
INSERT INTO bulk_load_test (content)
SELECT 'secondbatch' || i || ' more content for spill'
FROM generate_series(1, 200) AS i;
COMMIT;

-- Verify both batches are searchable
SELECT count(*) FROM bulk_load_test
WHERE content <@> to_bm25query('uniqueterm1', 'bulk_load_idx') < 0;

SELECT count(*) FROM bulk_load_test
WHERE content <@> to_bm25query('secondbatch1', 'bulk_load_idx') < 0;

-- Reset threshold
SET pg_textsearch.bulk_load_threshold = 100000;

-- Clean up
DROP TABLE bulk_load_test;
DROP EXTENSION pg_textsearch CASCADE;
