-- Additional coverage tests for untested code paths
-- Tests debug functions, segment dump, and text<@>text operator

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- =============================================================================
-- Test 1: Basic debug functions with memtable-only data
-- =============================================================================

-- Create a simple table and index
CREATE TABLE coverage_docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO coverage_docs (content) VALUES
    ('hello world test document'),
    ('database search query optimization'),
    ('full text search engine postgresql');

CREATE INDEX coverage_idx ON coverage_docs USING bm25(content)
    WITH (text_config='english');

-- Test bm25_summarize_index with memtable data
SELECT bm25_summarize_index('coverage_idx') IS NOT NULL AS summarize_memtable;

-- Test bm25_dump_index with memtable data
SELECT length(bm25_dump_index('coverage_idx')) > 0 AS dump_memtable;

-- =============================================================================
-- Test 2: Debug functions with segment data (exercises segment dump code)
-- =============================================================================

-- Force segment spill to have data on disk
SELECT bm25_spill_index('coverage_idx');

-- Test bm25_summarize_index with segment data
-- This exercises tp_summarize_index_to_output with segments
SELECT bm25_summarize_index('coverage_idx') IS NOT NULL AS summarize_segment;

-- Test bm25_dump_index with segment data
-- This exercises tp_dump_segment_to_output, read_dict_entry, read_term_at_index
SELECT length(bm25_dump_index('coverage_idx')) > 0 AS dump_segment;

-- =============================================================================
-- Test 3: bm25query equality (tpquery_eq)
-- =============================================================================

-- Test equality of identical queries
SELECT to_bm25query('hello', 'coverage_idx') = to_bm25query('hello', 'coverage_idx') AS same_query_eq;

-- Test inequality of different queries
SELECT to_bm25query('hello', 'coverage_idx') = to_bm25query('world', 'coverage_idx') AS diff_query_eq;

-- =============================================================================
-- Test 4: text <@> text operator (bm25_text_text_score)
-- This is the implicit form without explicit bm25query
-- =============================================================================

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
-- Test 5: Multiple segments (exercises segment iteration in dump)
-- =============================================================================

-- Add more data and spill again to create multiple segments
INSERT INTO coverage_docs (content)
SELECT 'document number ' || i || ' with varying content for coverage'
FROM generate_series(1, 100) AS i;

SELECT bm25_spill_index('coverage_idx');

-- Dump with multiple segments
SELECT length(bm25_dump_index('coverage_idx')) > 0 AS dump_multi_segment;

-- =============================================================================
-- Cleanup
-- =============================================================================

DROP TABLE coverage_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
