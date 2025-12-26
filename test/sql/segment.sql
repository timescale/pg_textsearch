-- Test case: segment
-- Tests segment query functionality with multiple spill cycles and
-- post-spill inserts
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\set ECHO none
\i test/sql/validation.sql
\set ECHO all

SET pg_textsearch.log_scores = false;
SET enable_seqscan = off;

-- Create table and index
CREATE TABLE segment_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE INDEX segment_test_idx ON segment_test USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

--------------------------------------------------------------------------------
-- Phase 1: Initial data in memtable
--------------------------------------------------------------------------------

INSERT INTO segment_test (content) VALUES ('hello world');
INSERT INTO segment_test (content) VALUES ('goodbye cruel world');
INSERT INTO segment_test (content) VALUES ('hello goodbye');
INSERT INTO segment_test (content) VALUES ('world peace');

-- Validate BM25 scoring before spill (memtable only)
SELECT 'Phase 1: memtable only' AS phase;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'world', 'english', 1.2, 0.75)
       AS world_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello world', 'english', 1.2, 0.75)
       AS hello_world_valid;

--------------------------------------------------------------------------------
-- Phase 2: First spill to segment
--------------------------------------------------------------------------------

SELECT bm25_spill_index('segment_test_idx') IS NOT NULL AS first_spill;

-- Validate BM25 scoring after first spill (segment only)
SELECT 'Phase 2: after first spill (segment only)' AS phase;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'world', 'english', 1.2, 0.75)
       AS world_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello world', 'english', 1.2, 0.75)
       AS hello_world_valid;

--------------------------------------------------------------------------------
-- Phase 3: Post-spill inserts (memtable + segment)
--------------------------------------------------------------------------------

INSERT INTO segment_test (content) VALUES ('hello again friend');
INSERT INTO segment_test (content) VALUES ('peaceful world harmony');
INSERT INTO segment_test (content) VALUES ('cruel goodbye forever');

-- Validate BM25 scoring with mixed data (segment + memtable)
SELECT 'Phase 3: post-spill inserts (segment + memtable)' AS phase;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'world', 'english', 1.2, 0.75)
       AS world_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'cruel', 'english', 1.2, 0.75)
       AS cruel_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello world', 'english', 1.2, 0.75)
       AS hello_world_valid;

--------------------------------------------------------------------------------
-- Phase 4: Second spill (two segments now)
--------------------------------------------------------------------------------

SELECT bm25_spill_index('segment_test_idx') IS NOT NULL AS second_spill;

-- Validate BM25 scoring after second spill (two segments)
SELECT 'Phase 4: after second spill (two segments)' AS phase;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'world', 'english', 1.2, 0.75)
       AS world_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'cruel', 'english', 1.2, 0.75)
       AS cruel_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello world', 'english', 1.2, 0.75)
       AS hello_world_valid;

--------------------------------------------------------------------------------
-- Phase 5: More inserts after second spill
--------------------------------------------------------------------------------

INSERT INTO segment_test (content) VALUES ('hello hello hello');
INSERT INTO segment_test (content) VALUES ('world world');
INSERT INTO segment_test (content) VALUES ('new unique term');

-- Validate with three data sources (two segments + memtable)
SELECT 'Phase 5: more inserts (two segments + memtable)' AS phase;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'world', 'english', 1.2, 0.75)
       AS world_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'unique', 'english', 1.2, 0.75)
       AS unique_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello world', 'english', 1.2, 0.75)
       AS hello_world_valid;

--------------------------------------------------------------------------------
-- Phase 6: Third spill and final validation
--------------------------------------------------------------------------------

SELECT bm25_spill_index('segment_test_idx') IS NOT NULL AS third_spill;

SELECT 'Phase 6: after third spill (three segments)' AS phase;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'world', 'english', 1.2, 0.75)
       AS world_valid;
SELECT validate_bm25_scoring('segment_test', 'content', 'segment_test_idx',
                             'hello world cruel peace', 'english', 1.2, 0.75)
       AS multi_term_valid;

--------------------------------------------------------------------------------
-- Verify row counts and final state
--------------------------------------------------------------------------------

SELECT COUNT(*) AS total_documents FROM segment_test;

-- Show final scores for reference (not validated, just for visibility)
SELECT id, content,
       ROUND((content <@> 'hello')::numeric,
             4) AS hello_score
FROM segment_test
ORDER BY content <@> 'hello', id;

-- Cleanup
DROP TABLE segment_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
