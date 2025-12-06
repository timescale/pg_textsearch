-- Test BM25 index behavior with table inheritance

-- Load pg_textsearch extension
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- =============================================================================
-- Test 1: PostgreSQL native table inheritance
-- BM25 indexes are not supported on tables with inheritance children
-- =============================================================================
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
CREATE INDEX parent_bm25_idx ON parent_docs USING bm25(content)
    WITH (text_config='english');
\set VERBOSITY default

-- BM25 index on child table should work
CREATE INDEX child1_bm25_idx ON child_docs1 USING bm25(content)
    WITH (text_config='english');

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

-- =============================================================================
-- Test 2: Regular tables (should work normally)
-- =============================================================================
CREATE TABLE regular_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO regular_docs (content) VALUES
    ('Regular table without inheritance'),
    ('Should work perfectly with BM25');

-- BM25 index on regular table should work
CREATE INDEX regular_bm25_idx ON regular_docs USING bm25(content)
    WITH (text_config='english');

-- Verify regular index works
SELECT bm25_dump_index('regular_bm25_idx')::text ~ 'total_docs: 2' as has_two_docs;

-- Cleanup
DROP TABLE regular_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
