-- BMW Multi-term Scoring Bug Test
--
-- This test exposes a critical bug in multi-term BMW scoring.
-- Documents appearing at different block positions across terms' posting
-- lists get partial scores instead of complete scores.
--
-- The bug: Block-index iteration processes blocks by index (0, 1, 2, ...)
-- assuming blocks are aligned by doc ID across terms. They're not.
-- A document in block 1 of "alpha" and block 3 of "beta" gets scored
-- twice with partial scores, neither of which is correct.

CREATE EXTENSION pg_textsearch;

-- ============================================================
-- TEST: Multi-term doc missing from top-k due to partial scores
-- ============================================================

CREATE TABLE bmw_bug (id SERIAL PRIMARY KEY, content TEXT);
CREATE INDEX bmw_bug_idx ON bmw_bug USING bm25(content) WITH (text_config='english');

-- Insert in order that puts multi-term doc at different block positions:
-- 200 alpha-only docs, then 1 multi-term, then 500 beta-only docs
INSERT INTO bmw_bug (content) SELECT 'alpha only ' || i FROM generate_series(1, 200) i;
INSERT INTO bmw_bug (content) VALUES ('alpha beta both terms here');  -- id=201
INSERT INTO bmw_bug (content) SELECT 'beta only ' || i FROM generate_series(202, 700) i;

-- Spill to segment (creates single segment with all 700 docs)
SELECT bm25_spill_index('bmw_bug_idx');

-- Posting list layout in segment:
-- "alpha": 201 postings (docs 1-200, 201)
--   - Block 0: docs 1-128
--   - Block 1: docs 129-200, 201 (doc 201 at position 200)
-- "beta": 500 postings (doc 201, docs 202-700)
--   - Block 0: docs 201, 202-329 (doc 201 at position 0)
--   - Block 1: docs 330-457
--   - Block 2: docs 458-585
--   - Block 3: docs 586-700
--
-- Doc 201 is at:
--   - "alpha" block 1 (near end of short list)
--   - "beta" block 0 (at start of long list)
--
-- When BMW processes by block_idx:
--   block 0: finds doc 201 in beta only -> partial score
--   block 1: finds doc 201 in alpha only -> partial score
-- Neither score is the correct combined score!

SELECT bm25_summarize_index('bmw_bug_idx');

-- Get ground truth with exhaustive scoring
SET pg_textsearch.enable_bmw = off;
CREATE TEMP TABLE exhaustive_results AS
SELECT id, content <@> 'alpha beta'::bm25query as score
FROM bmw_bug
WHERE content <@> 'alpha beta'::bm25query < 0
ORDER BY score LIMIT 10;

-- Get BMW results
SET pg_textsearch.enable_bmw = on;
CREATE TEMP TABLE bmw_results AS
SELECT id, content <@> 'alpha beta'::bm25query as score
FROM bmw_bug
WHERE content <@> 'alpha beta'::bm25query < 0
ORDER BY score LIMIT 10;

-- Show both result sets
SELECT 'exhaustive' as path, * FROM exhaustive_results ORDER BY score;
SELECT 'bmw' as path, * FROM bmw_results ORDER BY score;

-- TEST 1: Doc 201 should be in top 10 of exhaustive (it's the ONLY multi-term doc!)
SELECT 'exhaustive-has-201' as test,
    CASE WHEN 201 IN (SELECT id FROM exhaustive_results)
    THEN 'PASS' ELSE 'FAIL' END as result;

-- TEST 2: Doc 201 should be in top 10 of BMW (THIS FAILS - THE BUG!)
SELECT 'bmw-has-201' as test,
    CASE WHEN 201 IN (SELECT id FROM bmw_results)
    THEN 'PASS' ELSE 'FAIL - DOC 201 MISSING DUE TO PARTIAL SCORING BUG' END as result;

-- TEST 3: Doc 201 should be #1 in exhaustive (best score)
SELECT 'exhaustive-201-is-top' as test,
    CASE WHEN (SELECT id FROM exhaustive_results ORDER BY score LIMIT 1) = 201
    THEN 'PASS' ELSE 'FAIL' END as result;

-- TEST 4: Results should match between BMW and exhaustive
SELECT 'results-match' as test,
    CASE WHEN (SELECT COUNT(*) FROM
        (SELECT id FROM exhaustive_results EXCEPT SELECT id FROM bmw_results) x) = 0
    THEN 'PASS' ELSE 'FAIL - RESULTS DIFFER' END as result;

-- Show what score doc 201 SHOULD have
SET pg_textsearch.enable_bmw = off;
SELECT 'correct-score-for-201' as info,
    content <@> 'alpha beta'::bm25query as expected_score
FROM bmw_bug WHERE id = 201;

DROP TABLE exhaustive_results, bmw_results;
DROP TABLE bmw_bug;
DROP EXTENSION pg_textsearch CASCADE;
