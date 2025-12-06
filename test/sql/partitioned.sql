-- Test BM25 index behavior with partitioned tables

-- Load pg_textsearch extension
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- =============================================================================
-- Test 1: Basic partitioned table with BM25 index
-- When creating an index on a partitioned table, PostgreSQL automatically
-- creates indexes on all existing partitions. The parent partitioned table
-- itself doesn't call our index build functions, only the partitions do.
-- =============================================================================
CREATE TABLE partitioned_docs (
    id SERIAL,
    content TEXT,
    created_at TIMESTAMP
) PARTITION BY RANGE (created_at);

-- Create a partition first
CREATE TABLE partition_2024 PARTITION OF partitioned_docs
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');

-- Insert data into partition
INSERT INTO partition_2024 (content, created_at) VALUES
    ('Data in the partition', '2024-06-15');

-- Creating index on partitioned table will automatically create it on partitions
-- This creates an index with auto-generated name like partition_2024_content_idx
CREATE INDEX partitioned_bm25_idx ON partitioned_docs USING bm25(content)
    WITH (text_config='english');

-- Verify the auto-created partition index works (using the generated name)
SELECT indexrelid::regclass as index_name
FROM pg_index
WHERE indrelid = 'partition_2024'::regclass
  AND indexrelid::regclass::text LIKE '%content%';

-- Create another partition after index exists
CREATE TABLE partition_2025 PARTITION OF partitioned_docs
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

-- Insert data into new partition
INSERT INTO partition_2025 (content, created_at) VALUES
    ('Data in the 2025 partition', '2025-06-15');

-- Verify the auto-created index on new partition
SELECT COUNT(*) as num_partition_indexes
FROM pg_index
WHERE indrelid IN ('partition_2024'::regclass, 'partition_2025'::regclass);

-- =============================================================================
-- Test 2: Implicit index resolution with <@> operator on partitioned table
-- The planner hook should resolve partitioned_bm25_idx, and the executor
-- should map it to the correct partition index at scan time
-- =============================================================================
SET enable_seqscan = off;

-- Query partition_2024 - should use partition_2024_content_idx
EXPLAIN (COSTS OFF)
SELECT content FROM partitioned_docs
WHERE created_at >= '2024-01-01' AND created_at < '2025-01-01'
ORDER BY content <@> 'data'
LIMIT 1;

-- Actually execute to verify the query works
SELECT content FROM partitioned_docs
WHERE created_at >= '2024-01-01' AND created_at < '2025-01-01'
ORDER BY content <@> 'data'
LIMIT 1;

-- Query partition_2025 - should use partition_2025_content_idx
SELECT content FROM partitioned_docs
WHERE created_at >= '2025-01-01' AND created_at < '2026-01-01'
ORDER BY content <@> 'partition'
LIMIT 1;

SET enable_seqscan = on;

-- Cleanup
DROP TABLE partitioned_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
