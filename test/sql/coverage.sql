-- Additional coverage tests for untested code paths
-- Tests debug functions, binary I/O, and text<@>text operator

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- =============================================================================
-- Test 1: bm25_summarize_index debug function
-- =============================================================================

-- Create a simple table and index
CREATE TABLE coverage_docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO coverage_docs (content) VALUES
    ('hello world test document'),
    ('database search query optimization'),
    ('full text search engine postgresql');

CREATE INDEX coverage_idx ON coverage_docs USING bm25(content)
    WITH (text_config='english');

-- Test bm25_summarize_index - should return statistics without full dump
SELECT bm25_summarize_index('coverage_idx') IS NOT NULL AS summarize_works;

-- Test bm25_dump_index - basic call (string mode, truncated output)
SELECT length(bm25_dump_index('coverage_idx')) > 0 AS dump_works;

-- =============================================================================
-- Test 2: bm25query equality (tpquery_eq)
-- =============================================================================

-- Test equality of identical queries
SELECT to_bm25query('hello', 'coverage_idx') = to_bm25query('hello', 'coverage_idx') AS same_query_eq;

-- Test inequality of different queries
SELECT to_bm25query('hello', 'coverage_idx') = to_bm25query('world', 'coverage_idx') AS diff_query_eq;

-- =============================================================================
-- Test 3: text <@> text operator (bm25_text_text_score)
-- This is the implicit form without index context
-- =============================================================================

-- Force segment spill to have data on disk for the query
SELECT bm25_spill_index('coverage_idx');

-- Insert more data to create variation
INSERT INTO coverage_docs (content) VALUES
    ('additional text for testing'),
    ('more content with different words');

-- Test text <@> text implicit scoring
-- This uses find_first_child_index to locate the bm25 index
SET enable_seqscan = off;

-- The implicit form should find coverage_idx automatically
SELECT content, content <@> 'hello'::text AS score
FROM coverage_docs
WHERE content <@> 'hello'::text < 0
ORDER BY content <@> 'hello'::text
LIMIT 3;

SET enable_seqscan = on;

-- =============================================================================
-- Cleanup
-- =============================================================================

DROP TABLE coverage_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
