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
SELECT count(*) FROM vb_test
WHERE content <@> to_bm25query('databases', 'vb_idx') < 0;

-- Delete some rows
DELETE FROM vb_test WHERE id <= 20;

-- VACUUM to apply bitmap marking
VACUUM vb_test;

-- Query should return only live rows
SELECT count(*) FROM vb_test
WHERE content <@> to_bm25query('databases', 'vb_idx') < 0;

-- Verify no deleted rows appear
SELECT count(*) FROM (
    SELECT id FROM vb_test
    WHERE content <@> to_bm25query('databases', 'vb_idx') < 0
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
SELECT count(*) FROM vb_full
WHERE content <@> to_bm25query('fulldelete', 'vb_full_idx') < 0;

-- Index should still be usable after inserting new rows
INSERT INTO vb_full (content) VALUES ('fresh document about topics');

SELECT count(*) FROM vb_full
WHERE content <@> to_bm25query('fresh', 'vb_full_idx') < 0;

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
SELECT count(*) > 0 AS has_alpha FROM vb_reuse
WHERE content <@> to_bm25query('alpha', 'vb_reuse_idx') < 0;

-- Delete all, VACUUM (frees CTIDs for reuse)
DELETE FROM vb_reuse;
VACUUM vb_reuse;

-- Insert new rows (Postgres may reuse CTIDs)
INSERT INTO vb_reuse (content)
SELECT 'beta keyword document ' || i
FROM generate_series(1, 50) i;

-- alpha should return nothing
SELECT count(*) FROM vb_reuse
WHERE content <@> to_bm25query('alpha', 'vb_reuse_idx') < 0;

-- beta should return results
SELECT count(*) > 0 AS has_beta FROM vb_reuse
WHERE content <@> to_bm25query('beta', 'vb_reuse_idx') < 0;

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
SELECT count(*) FROM vb_multi
WHERE content <@> to_bm25query('search', 'vb_multi_idx') < 0;

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
SELECT count(*) FROM vb_merge
WHERE content <@> to_bm25query('merge', 'vb_merge_idx') < 0;

DROP TABLE vb_merge;

-- =============================================================
-- Test 6: Score correctness after deletes
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

-- Query with LIMIT (uses BMW)
SELECT id,
       content <@> to_bm25query('performance database',
                                'vb_scores_idx') AS score
FROM vb_scores
WHERE content <@> to_bm25query('performance database',
                               'vb_scores_idx') < 0
ORDER BY content <@> to_bm25query('performance database',
                                  'vb_scores_idx')
LIMIT 5;

DROP TABLE vb_scores;

DROP EXTENSION pg_textsearch;
