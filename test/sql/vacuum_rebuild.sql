-- Test: VACUUM correctly removes dead index entries so CTID reuse
-- does not produce false positives.
--
-- The bug only manifests via ORDER BY <@> LIMIT queries, which use
-- the index scan path. Seq scans re-evaluate the <@> operator on
-- the heap tuple and catch stale results. We use enable_seqscan=off
-- to ensure the index scan is used even for small tables.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET enable_seqscan = off;

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

-- Verify baseline: index scan for 'alpha' returns the right row
SELECT id, content FROM vacuum_rebuild_test
ORDER BY content <@> to_bm25query('alpha', 'vacuum_rebuild_idx')
LIMIT 1;

-- Delete rows containing 'alpha' and 'delta', then VACUUM to free
-- their CTIDs for reuse.
DELETE FROM vacuum_rebuild_test WHERE id <= 2;
VACUUM vacuum_rebuild_test;

-- Insert new rows. Postgres will reuse the freed CTIDs (0,1) and
-- (0,2) for these new rows.
INSERT INTO vacuum_rebuild_test (content) VALUES
    ('juliet kilo lima'),
    ('mike november oscar');

-- Critical test: ORDER BY <@> LIMIT uses the index scan path.
-- Without the fix, the stale 'alpha' index entry still points to
-- CTID (0,1), which is now 'juliet kilo lima'. The index scan
-- returns this wrong row. With the fix, VACUUM rebuilt the segment
-- without the dead entries, so 'alpha' returns no results.
SELECT count(*) AS alpha_false_positives
FROM (
    SELECT id FROM vacuum_rebuild_test
    ORDER BY content <@> to_bm25query('alpha', 'vacuum_rebuild_idx')
    LIMIT 10
) sub
WHERE id NOT IN (
    SELECT id FROM vacuum_rebuild_test
    WHERE content LIKE '%alpha%'
);

-- Search for new terms must work via index scan
SELECT id, content FROM vacuum_rebuild_test
ORDER BY content <@> to_bm25query('juliet', 'vacuum_rebuild_idx')
LIMIT 1;

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

-- Must NOT find phantom 'papaya' results via index scan
SELECT count(*) AS papaya_false_positives
FROM (
    SELECT id FROM vacuum_allgone
    ORDER BY content <@> to_bm25query('papaya', 'vacuum_allgone_idx')
    LIMIT 10
) sub
WHERE id NOT IN (
    SELECT id FROM vacuum_allgone
    WHERE content LIKE '%papaya%'
);

-- Must find the new row
SELECT id, content FROM vacuum_allgone
ORDER BY content <@> to_bm25query('strawberry',
    'vacuum_allgone_idx')
LIMIT 1;

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

-- Verify remaining docs are searchable via index scan
SELECT count(*) AS remaining
FROM (
    SELECT id FROM vacuum_multilevel
    ORDER BY content <@> to_bm25query('searchterm',
        'vacuum_multilevel_idx')
    LIMIT 2000
) sub;

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

SELECT id, content FROM vacuum_noop
ORDER BY content <@> to_bm25query('deleted', 'vacuum_noop_idx')
LIMIT 1;

DROP TABLE vacuum_noop;

RESET enable_seqscan;

DROP EXTENSION pg_textsearch CASCADE;
