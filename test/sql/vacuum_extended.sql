-- Extended vacuum tests for BM25 indexes
-- Exercises vacuum paths with segments, bulk deletes, and empty indexes

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- =============================================================================
-- Test 1: Vacuum with segments present
-- =============================================================================

CREATE TABLE vacuum_seg_test (id serial PRIMARY KEY, content text);
CREATE INDEX vacuum_seg_idx ON vacuum_seg_test USING bm25(content)
    WITH (text_config='english');

-- Insert data and force segment creation
INSERT INTO vacuum_seg_test (content)
SELECT 'vacuum segment test document number ' || i
FROM generate_series(1, 200) AS i;

SELECT bm25_spill_index('vacuum_seg_idx');

-- Delete half the documents
DELETE FROM vacuum_seg_test WHERE id <= 100;

-- VACUUM with segments on disk
VACUUM vacuum_seg_test;

-- Verify search still works after vacuum with segments
SELECT count(*) FROM vacuum_seg_test
WHERE content <@> to_bm25query('vacuum', 'vacuum_seg_idx') < 0;

-- =============================================================================
-- Test 2: VACUUM FULL with segments (forces index rebuild)
-- =============================================================================

\set VERBOSITY terse
VACUUM FULL vacuum_seg_test;
\set VERBOSITY default

-- Verify search works after VACUUM FULL rebuild
SELECT count(*) FROM vacuum_seg_test
WHERE content <@> to_bm25query('vacuum', 'vacuum_seg_idx') < 0;

DROP TABLE vacuum_seg_test;

-- =============================================================================
-- Test 3: Vacuum on empty index
-- =============================================================================

CREATE TABLE vacuum_empty_test (id serial PRIMARY KEY, content text);
CREATE INDEX vacuum_empty_idx ON vacuum_empty_test USING bm25(content)
    WITH (text_config='english');

-- Vacuum empty index (should be a no-op)
VACUUM vacuum_empty_test;

-- Insert one doc, delete it, vacuum
INSERT INTO vacuum_empty_test (content) VALUES ('single document');
DELETE FROM vacuum_empty_test;
VACUUM vacuum_empty_test;

DROP TABLE vacuum_empty_test;

-- =============================================================================
-- Test 4: Bulk delete from memtable (exercises tp_bulkdelete_memtable_ctids)
-- =============================================================================

CREATE TABLE vacuum_bulk_test (id serial PRIMARY KEY, content text);
CREATE INDEX vacuum_bulk_idx ON vacuum_bulk_test USING bm25(content)
    WITH (text_config='english');

-- Insert docs (stays in memtable)
INSERT INTO vacuum_bulk_test (content)
SELECT 'bulk delete test term' || (i % 10) || ' document ' || i
FROM generate_series(1, 50) AS i;

-- Delete most of them
DELETE FROM vacuum_bulk_test WHERE id > 5;

-- Vacuum to clean up memtable entries
VACUUM vacuum_bulk_test;

-- Verify remaining docs are searchable
SELECT count(*) FROM vacuum_bulk_test
WHERE content <@> to_bm25query('bulk', 'vacuum_bulk_idx') < 0;

DROP TABLE vacuum_bulk_test;

-- =============================================================================
-- Test 5: REINDEX with unflushed memtable data
--
-- Verifies that REINDEX correctly rebuilds from the heap even when
-- the memtable has unflushed entries from recent INSERTs.
-- =============================================================================

CREATE TABLE reindex_memtable_test (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX reindex_memtable_idx
    ON reindex_memtable_test USING bm25(content)
    WITH (text_config='english');

-- Phase 1: Initial data via CREATE INDEX (already built above)
INSERT INTO reindex_memtable_test (content)
SELECT 'original document about databases number ' || i
FROM generate_series(1, 50) AS i;

-- Force a spill so we have segments on disk
SELECT bm25_spill_index('reindex_memtable_idx');

-- Verify baseline: all 50 docs are searchable
SELECT count(*) AS pre_reindex_count FROM reindex_memtable_test
WHERE content <@> to_bm25query('databases', 'reindex_memtable_idx') < 0;

-- Phase 2: Insert more rows that stay in memtable (unflushed)
INSERT INTO reindex_memtable_test (content)
SELECT 'additional document about databases number ' || i
FROM generate_series(51, 75) AS i;

-- Verify memtable scan finds all 75 docs
SELECT count(*) AS with_memtable_count FROM reindex_memtable_test
WHERE content <@> to_bm25query('databases', 'reindex_memtable_idx') < 0;

-- Phase 3: REINDEX discards memtable, rebuilds from heap
\set VERBOSITY terse
REINDEX INDEX reindex_memtable_idx;
\set VERBOSITY default

-- All 75 docs must still be found (rebuilt from heap, not memtable)
SELECT count(*) AS post_reindex_count FROM reindex_memtable_test
WHERE content <@> to_bm25query('databases', 'reindex_memtable_idx') < 0;

-- Phase 4: Verify INSERTs still work after REINDEX
INSERT INTO reindex_memtable_test (content)
VALUES ('final document about databases after reindex');

SELECT count(*) AS final_count FROM reindex_memtable_test
WHERE content <@> to_bm25query('databases', 'reindex_memtable_idx') < 0;

DROP TABLE reindex_memtable_test;

-- Clean up
DROP EXTENSION pg_textsearch CASCADE;
