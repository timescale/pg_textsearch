-- Test BM25 index behavior with table inheritance and partitioning

-- Load pg_textsearch extension
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Test 1: PostgreSQL native table inheritance
-- Create parent and child tables
CREATE TABLE parent_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE TABLE child_docs1 (
    category TEXT DEFAULT 'category1'
) INHERITS (parent_docs);

CREATE TABLE child_docs2 (
    category TEXT DEFAULT 'category2'
) INHERITS (parent_docs);

-- Insert data into child tables
INSERT INTO child_docs1 (content) VALUES
    ('The quick brown fox jumps over the lazy dog'),
    ('PostgreSQL inheritance allows tables to share structure');

INSERT INTO child_docs2 (content) VALUES
    ('Database normalization reduces redundancy'),
    ('Indexes help improve query performance');

-- Verify parent sees inherited data
SELECT COUNT(*) as parent_count FROM parent_docs;
SELECT COUNT(*) as parent_only_count FROM ONLY parent_docs;

-- Try to create BM25 index on parent table (should fail)
\set VERBOSITY terse
CREATE INDEX parent_bm25_idx ON parent_docs USING bm25(content) WITH (text_config='english');
\set VERBOSITY default

-- BM25 index on child table should work
CREATE INDEX child1_bm25_idx ON child_docs1 USING bm25(content) WITH (text_config='english');

-- Verify child index was created successfully with correct document count
SELECT bm25_dump_index('child1_bm25_idx')::text ~ 'total_docs: 2' as has_two_docs;

-- Test searching on child table works
SELECT COUNT(*) as matching_docs
FROM child_docs1
WHERE content @@ to_tsquery('english', 'postgresql & inheritance');

-- Cleanup inheritance test
DROP TABLE child_docs1 CASCADE;
DROP TABLE child_docs2 CASCADE;
DROP TABLE parent_docs CASCADE;

-- Test 2: PostgreSQL partitioned tables
-- Note: When creating an index on a partitioned table, PostgreSQL automatically
-- creates indexes on all existing partitions. The parent partitioned table itself
-- doesn't call our index build functions, only the partitions do.
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
CREATE INDEX partitioned_bm25_idx ON partitioned_docs USING bm25(content) WITH (text_config='english');

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

-- Cleanup partitioned test
DROP TABLE partitioned_docs CASCADE;

-- Test 3: Regular tables (should work normally)
CREATE TABLE regular_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO regular_docs (content) VALUES
    ('Regular table without inheritance'),
    ('Should work perfectly with BM25');

-- BM25 index on regular table should work
CREATE INDEX regular_bm25_idx ON regular_docs USING bm25(content) WITH (text_config='english');

-- Verify regular index works
SELECT bm25_dump_index('regular_bm25_idx')::text ~ 'total_docs: 2' as has_two_docs;

-- Cleanup
DROP TABLE regular_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
