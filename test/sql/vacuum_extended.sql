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
SELECT count(*) FROM (
    SELECT 1 FROM vacuum_seg_test
    ORDER BY content <@> to_bm25query('vacuum', 'vacuum_seg_idx')
) sub;

-- =============================================================================
-- Test 2: VACUUM FULL with segments (forces index rebuild)
-- =============================================================================

\set VERBOSITY terse
VACUUM FULL vacuum_seg_test;
\set VERBOSITY default

-- Verify search works after VACUUM FULL rebuild
SELECT count(*) FROM (
    SELECT 1 FROM vacuum_seg_test
    ORDER BY content <@> to_bm25query('vacuum', 'vacuum_seg_idx')
) sub;

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
SELECT count(*) FROM (
    SELECT 1 FROM vacuum_bulk_test
    ORDER BY content <@> to_bm25query('bulk', 'vacuum_bulk_idx')
) sub;

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
SELECT count(*) AS pre_reindex_count FROM (
    SELECT 1 FROM reindex_memtable_test
    ORDER BY content <@> to_bm25query('databases', 'reindex_memtable_idx')
) sub;

-- Phase 2: Insert more rows that stay in memtable (unflushed)
INSERT INTO reindex_memtable_test (content)
SELECT 'additional document about databases number ' || i
FROM generate_series(51, 75) AS i;

-- Verify memtable scan finds all 75 docs
SELECT count(*) AS with_memtable_count FROM (
    SELECT 1 FROM reindex_memtable_test
    ORDER BY content <@> to_bm25query('databases', 'reindex_memtable_idx')
) sub;

-- Phase 3: REINDEX discards memtable, rebuilds from heap
\set VERBOSITY terse
REINDEX INDEX reindex_memtable_idx;
\set VERBOSITY default

-- All 75 docs must still be found (rebuilt from heap, not memtable)
SELECT count(*) AS post_reindex_count FROM (
    SELECT 1 FROM reindex_memtable_test
    ORDER BY content <@> to_bm25query('databases', 'reindex_memtable_idx')
) sub;

-- Phase 4: Verify INSERTs still work after REINDEX
INSERT INTO reindex_memtable_test (content)
VALUES ('final document about databases after reindex');

SELECT count(*) AS final_count FROM (
    SELECT 1 FROM reindex_memtable_test
    ORDER BY content <@> to_bm25query('databases', 'reindex_memtable_idx')
) sub;

DROP TABLE reindex_memtable_test;

-- =============================================================================
-- Test 6: Bulk load with unflushed memtable data
--
-- Verifies that a large INSERT into an already-indexed table correctly
-- triggers auto-spills from the memtable while preserving earlier
-- unflushed memtable entries.  Uses a low spill threshold so the test
-- triggers multiple spills without needing millions of rows.
-- =============================================================================

-- Lower spill threshold BEFORE creating the table so it applies to inserts
SET pg_textsearch.memtable_spill_threshold = 500;

CREATE TABLE bulk_memtable_test (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX bulk_memtable_idx
    ON bulk_memtable_test USING bm25(content)
    WITH (text_config='english');

-- Phase 1: Small insert that stays in memtable
INSERT INTO bulk_memtable_test (content)
SELECT 'initial document about networking topic ' || i
FROM generate_series(1, 10) AS i;

SELECT count(*) AS initial_count FROM (
    SELECT 1 FROM bulk_memtable_test
    ORDER BY content <@> to_bm25query('networking', 'bulk_memtable_idx')
) sub;

-- Phase 2: Bulk insert triggers multiple auto-spills
INSERT INTO bulk_memtable_test (content)
SELECT 'bulk loaded document about networking and databases number ' || i
FROM generate_series(11, 5000) AS i;

-- All 5000 docs must be found (initial memtable + spilled segments + tail)
SELECT count(*) AS after_bulk_count FROM (
    SELECT 1 FROM bulk_memtable_test
    ORDER BY content <@> to_bm25query('networking', 'bulk_memtable_idx')
) sub;

-- Verify segments were created by the auto-spill
SELECT bm25_summarize_index('bulk_memtable_idx') ~ 'Total:.*segments'
    AS has_segments;

-- Phase 3: More inserts after bulk load still work
INSERT INTO bulk_memtable_test (content)
VALUES ('final document about networking after bulk load');

SELECT count(*) AS final_count FROM (
    SELECT 1 FROM bulk_memtable_test
    ORDER BY content <@> to_bm25query('networking', 'bulk_memtable_idx')
) sub;

RESET pg_textsearch.memtable_spill_threshold;
DROP TABLE bulk_memtable_test;

-- =============================================================================
-- Test 7: VACUUM spills un-spilled memtable on insert-only tables (issue #333)
--
-- On insert-only workloads ambulkdelete is skipped (no dead tuples), so the
-- memtable spill that used to live only there never ran.  The docid-page
-- chain then grew unbounded and the first query after a server restart had
-- to re-tokenize every referenced heap tuple.  This test exercises the
-- amvacuumcleanup path that now performs the spill.
-- =============================================================================

CREATE TABLE vacuum_insert_only_test (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_insert_only_idx
    ON vacuum_insert_only_test USING bm25(content)
    WITH (text_config='english');

-- Populate the docid chain above TP_MIN_SPILL_POSTINGS (1000) but
-- below pg_textsearch.bulk_load_threshold so the spill only happens
-- via amvacuumcleanup.  2000 docs * ~4 terms each ≈ 8000 postings.
INSERT INTO vacuum_insert_only_test (content)
SELECT 'insert only document number ' || i
FROM generate_series(1, 2000) AS i;

-- Chain should be populated before VACUUM.
SELECT bm25_summarize_index('vacuum_insert_only_idx')
        ~ E'docids: 2000\n'
    AS chain_populated_before_vacuum;

-- No dead tuples here, so ambulkdelete is not invoked; only
-- amvacuumcleanup runs.  It must still drain the docid chain.
VACUUM vacuum_insert_only_test;

-- Chain must be fully drained and the memtable contents spilled to a segment.
SELECT bm25_summarize_index('vacuum_insert_only_idx')
        ~ E'docids: 0\n'
    AS chain_empty_after_vacuum;

-- Search still finds all 2000 docs.
SELECT count(*) AS post_vacuum_count FROM (
    SELECT 1 FROM vacuum_insert_only_test
    ORDER BY content
        <@> to_bm25query('document', 'vacuum_insert_only_idx')
) sub;

DROP TABLE vacuum_insert_only_test;

-- =============================================================================
-- Test 8: tiny memtable stays un-spilled across VACUUM (issue #333 guard)
-- =============================================================================
--
-- Below TP_MIN_SPILL_POSTINGS the VACUUM-cleanup spill is a no-op,
-- because writing a runt L0 segment would cost more in subsequent
-- compaction than chain replay saves.

CREATE TABLE vacuum_tiny_test (id serial PRIMARY KEY, content text);
CREATE INDEX vacuum_tiny_idx
    ON vacuum_tiny_test USING bm25(content)
    WITH (text_config='english');

-- ~50 docs * ~4 terms = ~200 postings, well below the guard.
INSERT INTO vacuum_tiny_test (content)
SELECT 'tiny doc ' || i FROM generate_series(1, 50) AS i;

VACUUM vacuum_tiny_test;

-- Chain is still present; no runt L0 segment was produced.
SELECT bm25_summarize_index('vacuum_tiny_idx')
        ~ E'docids: 50\n'
    AS chain_preserved_after_vacuum;

DROP TABLE vacuum_tiny_test;

-- =============================================================================
-- Test 9: dropping a memtable-spilled L0 segment keeps total_len sane
-- =============================================================================
--
-- Regression test.  tp_write_segment used to leave header.total_tokens
-- at the cumulative shared-atomic value instead of the per-segment
-- sum.  When VACUUM later dropped that segment (all docs dead) the
-- inflated header.total_tokens was subtracted from metap->total_len
-- and the atomic, clamping them to 0 (or wrapping the atomic) and
-- breaking avgdl in BM25 scoring for the surviving docs.
--
-- We populate two L0 segments with distinct id ranges, delete every
-- row that landed in the second one, and then run VACUUM.  Afterwards
-- a search over the surviving corpus must return real matches.

CREATE TABLE l0_total_len_test (id serial PRIMARY KEY, content text);
CREATE INDEX l0_total_len_idx ON l0_total_len_test USING bm25(content)
    WITH (text_config='english');

-- Segment 1: ids 1..50 (the survivors).
INSERT INTO l0_total_len_test (content)
SELECT 'alpha common term doc ' || i FROM generate_series(1, 50) AS i;
SELECT bm25_spill_index('l0_total_len_idx');

-- Segment 2: ids 51..100 (to be fully deleted).
INSERT INTO l0_total_len_test (content)
SELECT 'beta common term doc ' || i FROM generate_series(51, 100) AS i;
SELECT bm25_spill_index('l0_total_len_idx');

-- Wipe every row that went into segment 2, then VACUUM to drop it.
DELETE FROM l0_total_len_test WHERE id > 50;
VACUUM l0_total_len_test;

-- total_len must reflect only the surviving segment's tokens.  The
-- pre-fix path inflated header.total_tokens to the cumulative atomic
-- so the subtraction clamped total_len to 0.  A healthy avg_doc_len
-- is the sharpest regression signal.
SELECT bm25_summarize_index('l0_total_len_idx') ~ E'total_len: 250\n'
    AS total_len_matches_survivors,
       bm25_summarize_index('l0_total_len_idx') ~ E'avg_doc_len: 5\\.00\n'
    AS avgdl_sane;

DROP TABLE l0_total_len_test;

-- Clean up
DROP EXTENSION pg_textsearch CASCADE;
