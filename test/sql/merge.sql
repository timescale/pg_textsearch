-- Test case: merge
-- Tests segment merge functionality and queries across multiple segment levels.
--
-- This test exercises:
-- 1. Multiple L0 segments
-- 2. L0 -> L1 merge triggered by segments_per_level = 2
-- 3. Queries across multiple segment levels (L0, L1)
--
-- Known issue: Query code only searches L0 segments (level_heads[0]).
-- After merge, data in L1 won't be found until this is fixed.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\set ECHO none
\i test/sql/validation.sql
\set ECHO all

SET pg_textsearch.log_scores = false;
SET enable_seqscan = off;

-- Enable merge: 2 segments per level triggers L0->L1 merge on 2nd spill
SET pg_textsearch.segments_per_level = 2;

-- Create table and index
CREATE TABLE merge_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE INDEX merge_test_idx ON merge_test USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

--------------------------------------------------------------------------------
-- Phase 1: First batch - will become first L0 segment after spill
--------------------------------------------------------------------------------

INSERT INTO merge_test (content) VALUES ('hello world database');
INSERT INTO merge_test (content) VALUES ('goodbye cruel world');
INSERT INTO merge_test (content) VALUES ('hello goodbye friend');
INSERT INTO merge_test (content) VALUES ('world peace harmony');

-- Verify data is queryable in memtable
SELECT 'Phase 1: memtable only' AS phase;
SELECT COUNT(*) AS count_before_spill FROM merge_test
WHERE content <@> to_bm25query('hello', 'merge_test_idx') < 0;

-- First spill creates segment 1 in L0
SELECT bm25_spill_index('merge_test_idx') IS NOT NULL AS first_spill;

-- Verify data is still queryable from L0 segment
SELECT 'Phase 1b: after first spill (1 segment in L0)' AS phase;
SELECT COUNT(*) AS hello_count_after_first_spill FROM merge_test
WHERE content <@> to_bm25query('hello', 'merge_test_idx') < 0;

--------------------------------------------------------------------------------
-- Phase 2: Second batch - triggers L0->L1 merge (segments_per_level=2)
--------------------------------------------------------------------------------

INSERT INTO merge_test (content) VALUES ('database indexing query');
INSERT INTO merge_test (content) VALUES ('search engine optimization');
INSERT INTO merge_test (content) VALUES ('database world news');
INSERT INTO merge_test (content) VALUES ('goodbye database friend');

-- Second spill creates segment 2 in L0, which triggers merge to L1
-- After merge: L0 is empty (cleared), L1 has merged segment
SELECT bm25_spill_index('merge_test_idx') IS NOT NULL AS second_spill;

-- After merge: merged segment in L1, L0 is empty
SELECT 'Phase 2: after second spill + merge (L0 empty, 1 segment in L1)' AS phase;

-- Should find 2 documents with 'hello' (both in L1 merged segment)
SELECT COUNT(*) AS hello_count_after_merge FROM merge_test
WHERE content <@> to_bm25query('hello', 'merge_test_idx') < 0;

-- Should find 4 documents with 'database' (all in L1 merged segment)
SELECT COUNT(*) AS database_count_after_merge FROM merge_test
WHERE content <@> to_bm25query('database', 'merge_test_idx') < 0;

-- Validate BM25 scoring is correct across L1 data
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid_after_merge;
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'database', 'english', 1.2, 0.75)
       AS database_valid_after_merge;
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'world', 'english', 1.2, 0.75)
       AS world_valid_after_merge;

--------------------------------------------------------------------------------
-- Phase 3: Post-merge inserts (new memtable data + 1 L1 segment)
--------------------------------------------------------------------------------

INSERT INTO merge_test (content) VALUES ('hello new insertion');
INSERT INTO merge_test (content) VALUES ('database transaction log');

SELECT 'Phase 3: post-merge inserts (memtable + 1 L1 segment)' AS phase;

-- Should find 3 documents with 'hello' (2 in L1, 1 in memtable)
SELECT COUNT(*) AS hello_count_with_new_inserts FROM merge_test
WHERE content <@> to_bm25query('hello', 'merge_test_idx') < 0;

-- Should find 5 documents with 'database' (4 in L1, 1 in memtable)
SELECT COUNT(*) AS database_count_with_new_inserts FROM merge_test
WHERE content <@> to_bm25query('database', 'merge_test_idx') < 0;

-- Validate BM25 scoring with mixed sources (memtable + L1 segment)
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid_mixed;
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'database', 'english', 1.2, 0.75)
       AS database_valid_mixed;

--------------------------------------------------------------------------------
-- Phase 4: Verify total document count
--------------------------------------------------------------------------------

SELECT 'Phase 4: final verification' AS phase;
SELECT COUNT(*) AS total_documents FROM merge_test;

-- Show final scores for reference
SELECT id, content,
       ROUND((content <@> to_bm25query('database', 'merge_test_idx'))::numeric, 4)
       AS database_score
FROM merge_test
WHERE content <@> to_bm25query('database', 'merge_test_idx') < 0
ORDER BY content <@> to_bm25query('database', 'merge_test_idx'), id;

-- Cleanup
DROP TABLE merge_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
