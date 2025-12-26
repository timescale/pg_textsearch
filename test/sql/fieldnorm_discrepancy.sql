-- Test case: fieldnorm_discrepancy
-- Demonstrates BM25 score corruption after L0->L1 segment merge.
--
-- Issue: After merge, index returns different scores than SQL reference.
-- - Expected (SQL): 0.6931
-- - Actual (index): 0.8938
-- - Error: ~29%
--
-- Root cause: Merge corrupts corpus statistics (likely avg_length or total_docs).
-- Note: Same test WITHOUT merge passes - scores match perfectly.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;
\set ECHO none
\i test/sql/validation.sql
\set ECHO all

SET pg_textsearch.log_scores = false;
SET enable_seqscan = off;

-- Enable merge: 2 segments per level triggers L0->L1 merge on 2nd spill
SET pg_textsearch.segments_per_level = 2;

-- Create table and index
CREATE TABLE fieldnorm_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE INDEX fieldnorm_test_idx ON fieldnorm_test USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

--------------------------------------------------------------------------------
-- Phase 1: First batch -> first L0 segment
--------------------------------------------------------------------------------
INSERT INTO fieldnorm_test (content) VALUES ('hello world database');
INSERT INTO fieldnorm_test (content) VALUES ('goodbye cruel world');
INSERT INTO fieldnorm_test (content) VALUES ('hello goodbye friend');
INSERT INTO fieldnorm_test (content) VALUES ('world peace harmony');

SELECT bm25_spill_index('fieldnorm_test_idx') IS NOT NULL AS first_spill;

--------------------------------------------------------------------------------
-- Phase 2: Second batch -> triggers L0->L1 merge
--------------------------------------------------------------------------------
INSERT INTO fieldnorm_test (content) VALUES ('database indexing query');
INSERT INTO fieldnorm_test (content) VALUES ('search engine optimization');
INSERT INTO fieldnorm_test (content) VALUES ('database world news');
INSERT INTO fieldnorm_test (content) VALUES ('goodbye database friend');

-- This spill triggers merge - after this, scores are corrupted
SELECT bm25_spill_index('fieldnorm_test_idx') IS NOT NULL AS second_spill_triggers_merge;

--------------------------------------------------------------------------------
-- Phase 3: Post-merge inserts
--------------------------------------------------------------------------------
INSERT INTO fieldnorm_test (content) VALUES ('hello new insertion');
INSERT INTO fieldnorm_test (content) VALUES ('database transaction log');

--------------------------------------------------------------------------------
-- Demonstrate score corruption after merge
--------------------------------------------------------------------------------
SELECT 'Score corruption after L0->L1 merge:' AS note;

SELECT
    doc_id,
    content,
    ROUND(sql_bm25::numeric, 4) AS expected_score,
    ROUND(tapir_bm25::numeric, 4) AS actual_score,
    ROUND(difference::numeric, 4) AS error,
    matches_4dp AS valid
FROM debug_bm25_computation(
    'fieldnorm_test', 'content', 'fieldnorm_test_idx',
    'database', 'english', 1.2, 0.75
)
WHERE sql_bm25 > 0
ORDER BY doc_id;

-- This fails due to merge corruption
SELECT validate_bm25_scoring('fieldnorm_test', 'content', 'fieldnorm_test_idx',
                             'database', 'english', 1.2, 0.75)
       AS database_valid_after_merge;

-- Cleanup
DROP TABLE fieldnorm_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
