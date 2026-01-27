-- Test parallel compaction functionality
-- This test verifies that parallel segment compaction works correctly

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Enable parallel maintenance workers for compaction
SET max_parallel_maintenance_workers = 4;
SET maintenance_work_mem = '64MB';

-- Lower thresholds to trigger more frequent spills and compaction
SET pg_textsearch.memtable_spill_threshold = 10000;
SET pg_textsearch.segments_per_level = 4;

-- Create test table
CREATE TABLE test_parallel_compact (
    id serial PRIMARY KEY,
    content text
);

-- Insert enough data to create multiple segments
-- This should create at least 8 L0 segments to trigger parallel compaction
INSERT INTO test_parallel_compact (content)
SELECT 'document ' || i || ' contains words like ' ||
       md5(i::text) || ' and ' ||
       md5((i * 2)::text) || ' plus unique term doc' || i
FROM generate_series(1, 50000) i;

-- Create the BM25 index (this will create initial segments)
CREATE INDEX test_parallel_compact_idx ON test_parallel_compact
    USING bm25(content) WITH (text_config='english');

-- Check index summary before explicit compaction
SELECT bm25_summarize_index('test_parallel_compact_idx');

-- Force a spill to ensure segments are on disk
SELECT bm25_spill_index('test_parallel_compact_idx');

-- Insert more data to create additional segments
INSERT INTO test_parallel_compact (content)
SELECT 'additional document ' || i || ' with more words ' ||
       md5((i + 50000)::text) || ' and term additionaldoc' || i
FROM generate_series(1, 50000) i;

-- Force another spill
SELECT bm25_spill_index('test_parallel_compact_idx');

-- Check index summary after more data
SELECT bm25_summarize_index('test_parallel_compact_idx');

-- Test queries work correctly before any manual compaction
-- Query 1: Single term
SELECT COUNT(*) FROM test_parallel_compact
WHERE content <@> 'document' < 0;

-- Query 2: Multiple terms
SELECT COUNT(*) FROM test_parallel_compact
WHERE content <@> 'additional words' < 0;

-- Query 3: Specific unique term to verify data integrity
SELECT id, content FROM test_parallel_compact
WHERE content <@> 'doc1000' < 0
ORDER BY content <@> 'doc1000'
LIMIT 5;

-- Insert even more data to trigger cascading compaction (L0 -> L1 -> L2)
INSERT INTO test_parallel_compact (content)
SELECT 'cascade document ' || i || ' testing cascading compaction ' ||
       md5((i + 100000)::text) || ' unique cascadedoc' || i
FROM generate_series(1, 50000) i;

SELECT bm25_spill_index('test_parallel_compact_idx');

-- Final index summary
SELECT bm25_summarize_index('test_parallel_compact_idx');

-- Verify query results are still correct after compaction
SELECT COUNT(*) FROM test_parallel_compact
WHERE content <@> 'document' < 0;

SELECT COUNT(*) FROM test_parallel_compact
WHERE content <@> 'cascade' < 0;

-- Test with ORDER BY to verify BM25 scoring works correctly
SELECT id, content <@> 'cascade compaction' AS score
FROM test_parallel_compact
WHERE content <@> 'cascade compaction' < 0
ORDER BY content <@> 'cascade compaction'
LIMIT 5;

-- Clean up
DROP TABLE test_parallel_compact;

-- Test with serial compaction disabled (fallback case)
SET max_parallel_maintenance_workers = 0;
SET pg_textsearch.segments_per_level = 4;

CREATE TABLE test_serial_compact (
    id serial PRIMARY KEY,
    content text
);

INSERT INTO test_serial_compact (content)
SELECT 'serial document ' || i || ' with content ' || md5(i::text)
FROM generate_series(1, 20000) i;

CREATE INDEX test_serial_compact_idx ON test_serial_compact
    USING bm25(content) WITH (text_config='english');

SELECT bm25_spill_index('test_serial_compact_idx');
SELECT bm25_summarize_index('test_serial_compact_idx');

-- Verify queries work with serial compaction
SELECT COUNT(*) FROM test_serial_compact
WHERE content <@> 'serial document' < 0;

DROP TABLE test_serial_compact;

-- Reset settings
RESET max_parallel_maintenance_workers;
RESET maintenance_work_mem;
RESET pg_textsearch.memtable_spill_threshold;
RESET pg_textsearch.segments_per_level;

-- Clean up extension
DROP EXTENSION pg_textsearch CASCADE;
