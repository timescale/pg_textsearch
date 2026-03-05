-- Test: VACUUM correctly removes dead index entries so CTID reuse
-- does not produce false positives.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- ================================================================
-- Test 1: Basic CTID reuse after VACUUM
-- ================================================================

CREATE TABLE vacuum_rebuild_test (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_rebuild_idx
    ON vacuum_rebuild_test USING bm25(content)
    WITH (text_config = 'english');

-- Insert documents with distinctive terms
INSERT INTO vacuum_rebuild_test (content) VALUES
    ('alpha bravo charlie'),
    ('delta echo foxtrot'),
    ('golf hotel india');

-- Force to segments so VACUUM has something to rebuild
SELECT bm25_spill_index('vacuum_rebuild_idx');

-- Verify baseline search works
SELECT id, content FROM vacuum_rebuild_test
WHERE content <@> to_bm25query('alpha', 'vacuum_rebuild_idx') < 0
ORDER BY content <@> to_bm25query('alpha', 'vacuum_rebuild_idx');

-- Delete and VACUUM to free CTIDs
DELETE FROM vacuum_rebuild_test WHERE id <= 2;
VACUUM vacuum_rebuild_test;

-- Insert new rows (may reuse CTIDs from deleted rows)
INSERT INTO vacuum_rebuild_test (content) VALUES
    ('juliet kilo lima'),
    ('mike november oscar');

-- Critical: search for 'alpha' must NOT return new rows
-- (old index entry for 'alpha' pointed to a CTID now reused)
SELECT count(*) AS alpha_matches FROM vacuum_rebuild_test
WHERE content <@> to_bm25query('alpha', 'vacuum_rebuild_idx') < 0;

-- Search for new terms must work
SELECT count(*) AS juliet_matches FROM vacuum_rebuild_test
WHERE content <@> to_bm25query('juliet', 'vacuum_rebuild_idx') < 0;

DROP TABLE vacuum_rebuild_test;

-- ================================================================
-- Test 2: All docs in segment deleted (segment removed from chain)
-- ================================================================

CREATE TABLE vacuum_allgone (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_allgone_idx
    ON vacuum_allgone USING bm25(content)
    WITH (text_config = 'english');

INSERT INTO vacuum_allgone (content) VALUES
    ('papaya mango'),
    ('papaya kiwi');

SELECT bm25_spill_index('vacuum_allgone_idx');

-- Delete everything, vacuum
DELETE FROM vacuum_allgone;
VACUUM vacuum_allgone;

-- Insert fresh data
INSERT INTO vacuum_allgone (content) VALUES
    ('strawberry banana');

-- Must find only the new row, not phantom results
SELECT count(*) AS papaya_gone FROM vacuum_allgone
WHERE content <@> to_bm25query('papaya', 'vacuum_allgone_idx') < 0;

SELECT count(*) AS strawberry_found FROM vacuum_allgone
WHERE content <@> to_bm25query('strawberry', 'vacuum_allgone_idx') < 0;

DROP TABLE vacuum_allgone;

-- ================================================================
-- Test 3: Multi-level segments with deletions
-- ================================================================

SET pg_textsearch.memtable_spill_threshold = 200;

CREATE TABLE vacuum_multilevel (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_multilevel_idx
    ON vacuum_multilevel USING bm25(content)
    WITH (text_config = 'english');

-- Insert enough to create L0 segments and trigger compaction to L1
INSERT INTO vacuum_multilevel (content)
SELECT 'multilevel test document number ' || i ||
       ' with keyword searchterm'
FROM generate_series(1, 2000) AS i;

-- Verify we have segments at multiple levels
SELECT bm25_summarize_index('vacuum_multilevel_idx') ~ 'L1'
    AS has_l1_segments;

-- Delete half
DELETE FROM vacuum_multilevel WHERE id <= 1000;
VACUUM vacuum_multilevel;

-- Verify remaining docs are searchable
SELECT count(*) AS remaining FROM vacuum_multilevel
WHERE content <@> to_bm25query('searchterm',
    'vacuum_multilevel_idx') < 0;

RESET pg_textsearch.memtable_spill_threshold;
DROP TABLE vacuum_multilevel;

-- ================================================================
-- Test 4: VACUUM with no dead tuples (no-op)
-- ================================================================

CREATE TABLE vacuum_noop (
    id serial PRIMARY KEY,
    content text
);
CREATE INDEX vacuum_noop_idx
    ON vacuum_noop USING bm25(content)
    WITH (text_config = 'english');

INSERT INTO vacuum_noop (content) VALUES
    ('nothing deleted here');

SELECT bm25_spill_index('vacuum_noop_idx');
VACUUM vacuum_noop;

SELECT count(*) AS still_here FROM vacuum_noop
WHERE content <@> to_bm25query('deleted', 'vacuum_noop_idx') < 0;

DROP TABLE vacuum_noop;

DROP EXTENSION pg_textsearch CASCADE;
