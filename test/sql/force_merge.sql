-- Test case: force_merge
-- Tests bm25_force_merge() which merges all segments into one.
--
-- This test exercises:
-- 1. Multiple segments across levels
-- 2. Force merge into a single segment
-- 3. Queries return correct results after merge
-- 4. Inserts work normally after force merge

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\set ECHO none
\i test/sql/validation.sql
\set ECHO all

SET pg_textsearch.log_scores = false;
SET enable_seqscan = off;

-- Use 2 segments per level so we get multi-level segments quickly
SET pg_textsearch.segments_per_level = 2;

CREATE TABLE force_merge_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE INDEX force_merge_idx ON force_merge_test USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

--------------------------------------------------------------------------------
-- Build up segments across multiple levels
--------------------------------------------------------------------------------

-- Batch 1 -> spill to L0
INSERT INTO force_merge_test (content) VALUES
    ('hello world database'),
    ('goodbye cruel world'),
    ('hello goodbye friend'),
    ('world peace harmony');
SELECT bm25_spill_index('force_merge_idx') IS NOT NULL AS spill1;

-- Batch 2 -> spill to L0, triggers L0->L1 merge
INSERT INTO force_merge_test (content) VALUES
    ('database indexing query'),
    ('search engine optimization'),
    ('database world news'),
    ('goodbye database friend');
SELECT bm25_spill_index('force_merge_idx') IS NOT NULL AS spill2;

-- Batch 3 -> new L0 segment (so we have L0 + L1)
INSERT INTO force_merge_test (content) VALUES
    ('hello search results'),
    ('optimization database tuning');
SELECT bm25_spill_index('force_merge_idx') IS NOT NULL AS spill3;

-- Verify counts before force merge
SELECT COUNT(*) AS hello_before FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('hello', 'force_merge_idx')
    LIMIT 100
) t;

SELECT COUNT(*) AS database_before FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('database', 'force_merge_idx')
    LIMIT 100
) t;

--------------------------------------------------------------------------------
-- Force merge all segments into one
--------------------------------------------------------------------------------

SELECT bm25_force_merge('force_merge_idx');

-- Verify same counts after force merge
SELECT COUNT(*) AS hello_after FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('hello', 'force_merge_idx')
    LIMIT 100
) t;

SELECT COUNT(*) AS database_after FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('database', 'force_merge_idx')
    LIMIT 100
) t;

-- Validate BM25 scoring is correct after force merge
SELECT validate_bm25_scoring('force_merge_test', 'content',
                             'force_merge_idx', 'hello', 'english',
                             1.2, 0.75) AS hello_scores_valid;
SELECT validate_bm25_scoring('force_merge_test', 'content',
                             'force_merge_idx', 'database', 'english',
                             1.2, 0.75) AS database_scores_valid;

--------------------------------------------------------------------------------
-- Verify inserts work after force merge
--------------------------------------------------------------------------------

INSERT INTO force_merge_test (content) VALUES
    ('hello after merge'),
    ('new database entry');

SELECT COUNT(*) AS hello_with_new FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('hello', 'force_merge_idx')
    LIMIT 100
) t;

SELECT validate_bm25_scoring('force_merge_test', 'content',
                             'force_merge_idx', 'hello', 'english',
                             1.2, 0.75) AS hello_valid_after_insert;

--------------------------------------------------------------------------------
-- Cleanup
--------------------------------------------------------------------------------

DROP TABLE force_merge_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
