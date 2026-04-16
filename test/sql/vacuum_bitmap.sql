-- Test alive bitset tombstone tracking during VACUUM

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- =============================================================
-- Test 1: Basic VACUUM with bitmap marking
-- =============================================================
CREATE TABLE vb_test (id serial PRIMARY KEY, content text);

INSERT INTO vb_test (content)
SELECT 'document number ' || i || ' about databases'
FROM generate_series(1, 100) i;

CREATE INDEX vb_idx ON vb_test
    USING bm25 (content) WITH (text_config = 'english');

-- Verify initial query works
SELECT count(*) FROM (
    SELECT id FROM vb_test
    ORDER BY content <@> to_bm25query('databases', 'vb_idx')
    LIMIT 1000
) q;

-- Delete some rows
DELETE FROM vb_test WHERE id <= 20;

-- VACUUM to apply bitmap marking
VACUUM vb_test;

-- Query should return only live rows
SELECT count(*) FROM (
    SELECT id FROM vb_test
    ORDER BY content <@> to_bm25query('databases', 'vb_idx')
    LIMIT 1000
) q;

-- Verify no deleted rows appear
SELECT count(*) FROM (
    SELECT id FROM vb_test
    ORDER BY content <@> to_bm25query('databases', 'vb_idx')
    LIMIT 1000
) q WHERE id <= 20;

DROP TABLE vb_test;

-- =============================================================
-- Test 2: Full segment delete — segment should be dropped
-- =============================================================
CREATE TABLE vb_full (id serial PRIMARY KEY, content text);

INSERT INTO vb_full (content)
SELECT 'fulldelete document ' || i
FROM generate_series(1, 50) i;

CREATE INDEX vb_full_idx ON vb_full
    USING bm25 (content) WITH (text_config = 'english');

DELETE FROM vb_full;
VACUUM vb_full;

-- Should return 0 results
SELECT count(*) FROM (
    SELECT id FROM vb_full
    ORDER BY content <@> to_bm25query('fulldelete', 'vb_full_idx')
    LIMIT 1000
) q;

-- Index should still be usable after inserting new rows
INSERT INTO vb_full (content) VALUES ('fresh document about topics');

SELECT count(*) FROM (
    SELECT id FROM vb_full
    ORDER BY content <@> to_bm25query('fresh', 'vb_full_idx')
    LIMIT 1000
) q;

DROP TABLE vb_full;

-- =============================================================
-- Test 3: CTID reuse correctness
-- =============================================================
CREATE TABLE vb_reuse (id serial PRIMARY KEY, content text);

INSERT INTO vb_reuse (content)
SELECT 'alpha keyword document ' || i
FROM generate_series(1, 50) i;

CREATE INDEX vb_reuse_idx ON vb_reuse
    USING bm25 (content) WITH (text_config = 'english');

-- Verify alpha matches
SELECT count(*) > 0 AS has_alpha FROM (
    SELECT id FROM vb_reuse
    ORDER BY content <@> to_bm25query('alpha', 'vb_reuse_idx')
    LIMIT 1000
) q;

-- Delete all, VACUUM (frees CTIDs for reuse)
DELETE FROM vb_reuse;
VACUUM vb_reuse;

-- Insert new rows (Postgres may reuse CTIDs)
INSERT INTO vb_reuse (content)
SELECT 'beta keyword document ' || i
FROM generate_series(1, 50) i;

-- alpha should return nothing
SELECT count(*) FROM (
    SELECT id FROM vb_reuse
    ORDER BY content <@> to_bm25query('alpha', 'vb_reuse_idx')
    LIMIT 1000
) q;

-- beta should return results
SELECT count(*) > 0 AS has_beta FROM (
    SELECT id FROM vb_reuse
    ORDER BY content <@> to_bm25query('beta', 'vb_reuse_idx')
    LIMIT 1000
) q;

DROP TABLE vb_reuse;

-- =============================================================
-- Test 4: Multiple segments with mixed deletes
-- =============================================================
CREATE TABLE vb_multi (id serial PRIMARY KEY, content text);

INSERT INTO vb_multi (content)
SELECT 'first batch document ' || i || ' with search term'
FROM generate_series(1, 50) i;

CREATE INDEX vb_multi_idx ON vb_multi
    USING bm25 (content) WITH (text_config = 'english');

-- Force first batch to segment
SELECT bm25_spill_index('vb_multi_idx');

-- Insert second batch
INSERT INTO vb_multi (content)
SELECT 'second batch document ' || i || ' with search term'
FROM generate_series(1, 50) i;

-- Force second batch to segment
SELECT bm25_spill_index('vb_multi_idx');

-- Delete from first batch only
DELETE FROM vb_multi WHERE id <= 25;

VACUUM vb_multi;

-- Should find docs from both batches (minus deleted)
SELECT count(*) FROM (
    SELECT id FROM vb_multi
    ORDER BY content <@> to_bm25query('search', 'vb_multi_idx')
    LIMIT 1000
) q;

DROP TABLE vb_multi;

-- =============================================================
-- Test 5: Merge after VACUUM cleans up dead docs
-- =============================================================
CREATE TABLE vb_merge (id serial PRIMARY KEY, content text);

INSERT INTO vb_merge (content)
SELECT 'merge test document ' || i
FROM generate_series(1, 50) i;

CREATE INDEX vb_merge_idx ON vb_merge
    USING bm25 (content) WITH (text_config = 'english');

-- Delete half
DELETE FROM vb_merge WHERE id <= 25;
VACUUM vb_merge;

-- Force merge to clean up dead docs
SELECT bm25_force_merge('vb_merge_idx');

-- Query should still work correctly
SELECT count(*) FROM (
    SELECT id FROM vb_merge
    ORDER BY content <@> to_bm25query('merge', 'vb_merge_idx')
    LIMIT 1000
) q;

DROP TABLE vb_merge;

-- =============================================================
-- Test 6: Score correctness after deletes (multi-term BMW)
-- =============================================================
CREATE TABLE vb_scores (id serial PRIMARY KEY, content text);

INSERT INTO vb_scores (content) VALUES
    ('database systems performance tuning'),
    ('performance optimization techniques'),
    ('database indexing strategies'),
    ('query optimization in databases'),
    ('systems programming fundamentals'),
    ('advanced database concepts'),
    ('delete me not relevant'),
    ('also delete me irrelevant'),
    ('performance database benchmark'),
    ('indexing performance metrics');

CREATE INDEX vb_scores_idx ON vb_scores
    USING bm25 (content) WITH (text_config = 'english');

-- Delete some rows
DELETE FROM vb_scores WHERE content LIKE '%delete me%';
VACUUM vb_scores;

-- Multi-term query via BMW index scan
SELECT id,
       content <@> to_bm25query('performance database',
                                'vb_scores_idx') AS score
FROM vb_scores
ORDER BY content <@> to_bm25query('performance database',
                                  'vb_scores_idx')
LIMIT 5;

DROP TABLE vb_scores;

-- =============================================================
-- Test 7: Many deletes (>64 per segment, exercises realloc)
-- =============================================================
CREATE TABLE vb_many (id serial PRIMARY KEY, content text);

INSERT INTO vb_many (content)
SELECT 'manydelete document ' || i || ' searchable'
FROM generate_series(1, 200) i;

CREATE INDEX vb_many_idx ON vb_many
    USING bm25 (content) WITH (text_config = 'english');

-- Delete 150 of 200 rows (well past the initial 64-element array)
DELETE FROM vb_many WHERE id <= 150;
VACUUM vb_many;

SELECT count(*) FROM (
    SELECT id FROM vb_many
    ORDER BY content <@> to_bm25query('searchable', 'vb_many_idx')
    LIMIT 1000
) q;

DROP TABLE vb_many;

-- =============================================================
-- Test 8: Multi-segment merge with dead docs
-- =============================================================
CREATE TABLE vb_multimerge (id serial PRIMARY KEY, content text);

INSERT INTO vb_multimerge (content)
SELECT 'batch1 doc ' || i || ' target'
FROM generate_series(1, 50) i;

CREATE INDEX vb_mm_idx ON vb_multimerge
    USING bm25 (content) WITH (text_config = 'english');

SELECT bm25_spill_index('vb_mm_idx');

INSERT INTO vb_multimerge (content)
SELECT 'batch2 doc ' || i || ' target'
FROM generate_series(1, 50) i;

SELECT bm25_spill_index('vb_mm_idx');

INSERT INTO vb_multimerge (content)
SELECT 'batch3 doc ' || i || ' target'
FROM generate_series(1, 50) i;

SELECT bm25_spill_index('vb_mm_idx');

-- Delete from each batch
DELETE FROM vb_multimerge WHERE id <= 10;
DELETE FROM vb_multimerge WHERE id > 50 AND id <= 60;
DELETE FROM vb_multimerge WHERE id > 100 AND id <= 110;

VACUUM vb_multimerge;

-- Merge all segments — exercises N-way merge with dead docs
SELECT bm25_force_merge('vb_mm_idx');

-- 150 total - 30 deleted = 120 live
SELECT count(*) FROM (
    SELECT id FROM vb_multimerge
    ORDER BY content <@> to_bm25query('target', 'vb_mm_idx')
    LIMIT 1000
) q;

DROP TABLE vb_multimerge;

-- =============================================================
-- Test 9: All docs dead across segments then merge
-- =============================================================
CREATE TABLE vb_alldead (id serial PRIMARY KEY, content text);

INSERT INTO vb_alldead (content)
SELECT 'alldead doc ' || i FROM generate_series(1, 30) i;

CREATE INDEX vb_alldead_idx ON vb_alldead
    USING bm25 (content) WITH (text_config = 'english');

SELECT bm25_spill_index('vb_alldead_idx');

INSERT INTO vb_alldead (content)
SELECT 'alldead second ' || i FROM generate_series(1, 30) i;

SELECT bm25_spill_index('vb_alldead_idx');

DELETE FROM vb_alldead;
VACUUM vb_alldead;

-- Both segments should be dropped — merge is a no-op
SELECT bm25_force_merge('vb_alldead_idx');

SELECT count(*) FROM (
    SELECT id FROM vb_alldead
    ORDER BY content <@> to_bm25query('alldead', 'vb_alldead_idx')
    LIMIT 1000
) q;

-- Insert fresh data to verify index still works
INSERT INTO vb_alldead (content) VALUES ('revival document here');

SELECT count(*) FROM (
    SELECT id FROM vb_alldead
    ORDER BY content <@> to_bm25query('revival', 'vb_alldead_idx')
    LIMIT 1000
) q;

DROP TABLE vb_alldead;

-- =============================================================
-- Test 10: Dump shows alive bitset info
-- =============================================================
CREATE TABLE vb_dump (id serial PRIMARY KEY, content text);

INSERT INTO vb_dump (content)
SELECT 'dump test ' || i FROM generate_series(1, 10) i;

CREATE INDEX vb_dump_idx ON vb_dump
    USING bm25 (content) WITH (text_config = 'english');

-- Verify dump output includes alive bitset info
SELECT bm25_dump_index('vb_dump_idx') LIKE '%Alive: 10 / 10 docs%'
    AS has_alive_info;

DROP TABLE vb_dump;

DROP EXTENSION pg_textsearch;
