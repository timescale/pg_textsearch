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
-- Note: With build mode, data is already spilled during CREATE INDEX,
-- so memtable is empty here and returns NULL
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
-- Test 6: Page visualization (exercises dump.c pageviz code)
-- =============================================================================

-- bm25_debug_pageviz writes page layout to file
SELECT bm25_debug_pageviz('coverage_idx', '/tmp/test_pageviz.txt');

-- Verify it wrote something
\! test -s /tmp/test_pageviz.txt && echo 'pageviz file exists'

-- Clean up temp file
\! rm -f /tmp/test_pageviz.txt

-- =============================================================================
-- Test 7: Score logging (exercises am/scan.c log_scores path)
-- =============================================================================

SET pg_textsearch.log_scores = true;

SELECT content <@> to_bm25query('hello', 'coverage_idx') AS score
FROM coverage_docs
WHERE content <@> to_bm25query('hello', 'coverage_idx') < 0
ORDER BY content <@> to_bm25query('hello', 'coverage_idx')
LIMIT 1;

SET pg_textsearch.log_scores = false;

-- =============================================================================
-- Test 8: BMW stats logging (exercises score.c bmw_stats path)
-- =============================================================================

SET pg_textsearch.log_bmw_stats = true;

-- Single-term query
SELECT count(*) FROM coverage_docs
WHERE content <@> to_bm25query('hello', 'coverage_idx') < 0;

-- Multi-term query
SELECT count(*) FROM coverage_docs
WHERE content <@> to_bm25query('hello world', 'coverage_idx') < 0;

SET pg_textsearch.log_bmw_stats = false;

-- =============================================================================
-- Test 9: AM handler property callbacks (exercises handler.c tp_property)
-- =============================================================================

-- Test pg_indexam_has_property for our access method
SELECT pg_indexam_has_property(am.oid, 'can_order') AS can_order,
       pg_indexam_has_property(am.oid, 'can_unique') AS can_unique
FROM pg_am am WHERE am.amname = 'bm25';

-- Test index-level properties
SELECT pg_index_has_property('coverage_idx'::regclass, 'clusterable') AS clusterable,
       pg_index_has_property('coverage_idx'::regclass, 'index_scan') AS index_scan;

-- Test column-level property (distance_orderable triggers tp_property)
SELECT pg_index_column_has_property('coverage_idx'::regclass, 1,
    'distance_orderable') AS dist_orderable;

-- =============================================================================
-- Test 10: AM validate error path (exercises handler.c tp_validate)
-- =============================================================================

-- Try creating a bm25 index on integer column (should fail validation)
CREATE TABLE validate_test (id serial, num integer);
CREATE INDEX validate_test_idx ON validate_test USING bm25(num)
    WITH (text_config='english');
DROP TABLE validate_test;

-- =============================================================================
-- Test 11: bm25_dump_index to file (exercises file output path)
-- =============================================================================

SELECT bm25_dump_index('coverage_idx', '/tmp/test_dump.txt') IS NOT NULL
    AS dump_to_file;

\! test -s /tmp/test_dump.txt && echo 'dump file exists'
\! rm -f /tmp/test_dump.txt

-- =============================================================================
-- Test 12: Segment BMW seek (exercises segment/scan.c seek path)
-- Uses ORDER BY ... LIMIT to trigger top-k BMW with segment data
-- =============================================================================

-- We already have segment data from Test 5; add more for multi-block
INSERT INTO coverage_docs (content)
SELECT 'seek test document with term alpha ' || i
FROM generate_series(1, 200) AS i;

SELECT bm25_spill_index('coverage_idx');

-- ORDER BY score LIMIT triggers BMW seek path on segments
SELECT id FROM coverage_docs
WHERE content <@> to_bm25query('alpha', 'coverage_idx') < 0
ORDER BY content <@> to_bm25query('alpha', 'coverage_idx')
LIMIT 5;

-- Multi-term BMW seek with segment data
SELECT id FROM coverage_docs
WHERE content <@> to_bm25query('seek alpha', 'coverage_idx') < 0
ORDER BY content <@> to_bm25query('seek alpha', 'coverage_idx')
LIMIT 3;

-- =============================================================================
-- Test 13: tpquery_in with index name prefix (exercises query.c parsing)
-- =============================================================================

-- Cast string with index:query format to bm25query type
SELECT 'coverage_idx:hello world'::bm25query IS NOT NULL
    AS tpquery_in_with_index;

-- =============================================================================
-- Test 14: BMW stats logging with segment ORDER BY LIMIT queries
-- =============================================================================

SET pg_textsearch.log_bmw_stats = true;

-- Single-term with ORDER BY LIMIT (BMW seek on segments)
SELECT count(*) FROM (
    SELECT id FROM coverage_docs
    WHERE content <@> to_bm25query('alpha', 'coverage_idx') < 0
    ORDER BY content <@> to_bm25query('alpha', 'coverage_idx')
    LIMIT 10
) sub;

-- Multi-term with ORDER BY LIMIT
SELECT count(*) FROM (
    SELECT id FROM coverage_docs
    WHERE content <@> to_bm25query('seek alpha', 'coverage_idx') < 0
    ORDER BY content <@> to_bm25query('seek alpha', 'coverage_idx')
    LIMIT 10
) sub;

SET pg_textsearch.log_bmw_stats = false;

-- =============================================================================
-- Cleanup
-- =============================================================================

DROP TABLE coverage_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
