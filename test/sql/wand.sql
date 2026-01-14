-- BMW Multi-term WAND Traversal Test
--
-- Regression test for doc-ID ordered traversal in multi-term queries.
-- Ensures documents at different block positions across posting lists
-- are correctly scored.

CREATE EXTENSION pg_textsearch;

-- ============================================================
-- TEST: Multi-term documents score correctly across block boundaries
-- ============================================================

CREATE TABLE bmw_bug (id SERIAL PRIMARY KEY, content TEXT);
CREATE INDEX bmw_bug_idx ON bmw_bug USING bm25(content) WITH (text_config='english');

-- Insert in order that puts multi-term doc at different block positions:
-- 5 alpha-only docs, then 1 multi-term, then 200 beta-only docs
-- Using smaller counts to avoid tie-breaking issues while still testing block traversal
INSERT INTO bmw_bug (content) SELECT 'alpha only ' || i FROM generate_series(1, 5) i;
INSERT INTO bmw_bug (content) VALUES ('alpha beta both terms here');  -- id=6
INSERT INTO bmw_bug (content) SELECT 'beta only ' || i FROM generate_series(7, 206) i;

-- Spill to segment (creates single segment with all 206 docs)
SELECT bm25_spill_index('bmw_bug_idx');

-- Posting list layout in segment:
-- "alpha": 6 postings (docs 1-5, 6) - fits in single block
-- "beta": 6 postings (doc 6, docs 7-206)
--   - Block 0: docs 6-133
--   - Block 1: docs 134-206
--
-- Doc 6 is the only multi-term document.

-- Get ground truth with exhaustive scoring
SET pg_textsearch.enable_bmw = off;
CREATE TEMP TABLE exhaustive_results AS
SELECT id, content <@> to_bm25query('alpha beta', 'bmw_bug_idx') as score
FROM bmw_bug
WHERE content <@> to_bm25query('alpha beta', 'bmw_bug_idx') < 0
ORDER BY score LIMIT 10;

-- Get BMW results
SET pg_textsearch.enable_bmw = on;
CREATE TEMP TABLE bmw_results AS
SELECT id, content <@> to_bm25query('alpha beta', 'bmw_bug_idx') as score
FROM bmw_bug
WHERE content <@> to_bm25query('alpha beta', 'bmw_bug_idx') < 0
ORDER BY score LIMIT 10;

-- Show both result sets
SELECT 'exhaustive' as path, * FROM exhaustive_results ORDER BY score;
SELECT 'bmw' as path, * FROM bmw_results ORDER BY score;

-- TEST 1: Doc 6 should be in top 10 of exhaustive (it's the ONLY multi-term doc!)
SELECT 'exhaustive-has-6' as test,
    CASE WHEN 6 IN (SELECT id FROM exhaustive_results)
    THEN 'PASS' ELSE 'FAIL' END as result;

-- TEST 2: Doc 6 should be in top 10 of BMW (THIS FAILS - THE BUG!)
SELECT 'bmw-has-6' as test,
    CASE WHEN 6 IN (SELECT id FROM bmw_results)
    THEN 'PASS' ELSE 'FAIL - DOC 6 MISSING DUE TO PARTIAL SCORING BUG' END as result;

-- TEST 3: Results should match between BMW and exhaustive
SELECT 'results-match' as test,
    CASE WHEN (SELECT COUNT(*) FROM
        (SELECT id FROM exhaustive_results EXCEPT SELECT id FROM bmw_results) x) = 0
    THEN 'PASS' ELSE 'FAIL - RESULTS DIFFER' END as result;

-- Show what score doc 6 SHOULD have
SET pg_textsearch.enable_bmw = off;
SELECT 'correct-score-for-6' as info,
    content <@> to_bm25query('alpha beta', 'bmw_bug_idx') as expected_score
FROM bmw_bug WHERE id = 6;

DROP TABLE exhaustive_results, bmw_results;
DROP TABLE bmw_bug;

-- ============================================================
-- TEST: 3-term query with multiple blocks (buffer management)
-- ============================================================
-- This tests a bug where zero-copy buffer pins were shared with the
-- reader's cached buffer. When initializing multiple term iterators,
-- reading the dictionary for term N could release the buffer that
-- term N-1's iterator was still using.

CREATE TABLE three_term (id SERIAL PRIMARY KEY, content TEXT);
CREATE INDEX three_term_idx ON three_term USING bm25(content)
    WITH (text_config='english');

-- Create layout to test multi-term scoring without excessive ties
-- Small counts to avoid tie-breaking issues
INSERT INTO three_term (content)
    SELECT 'alpha only ' || i FROM generate_series(1, 3) i;
INSERT INTO three_term (content)
    VALUES ('alpha beta two terms');  -- id=4
INSERT INTO three_term (content)
    SELECT 'beta only ' || i FROM generate_series(5, 7) i;
INSERT INTO three_term (content)
    VALUES ('alpha beta gamma three terms here');  -- id=8
INSERT INTO three_term (content)
    SELECT 'gamma only ' || i FROM generate_series(9, 11) i;

SELECT bm25_spill_index('three_term_idx');

-- Get ground truth with exhaustive scoring
-- Use LIMIT 20 to get all matching docs (only 11 in table)
SET pg_textsearch.enable_bmw = off;
CREATE TEMP TABLE ex_3term AS
SELECT id, content <@> to_bm25query('alpha beta gamma', 'three_term_idx') as score
FROM three_term
WHERE content <@> to_bm25query('alpha beta gamma', 'three_term_idx') < 0
ORDER BY score LIMIT 20;

-- Get BMW results
SET pg_textsearch.enable_bmw = on;
CREATE TEMP TABLE bmw_3term AS
SELECT id, content <@> to_bm25query('alpha beta gamma', 'three_term_idx') as score
FROM three_term
WHERE content <@> to_bm25query('alpha beta gamma', 'three_term_idx') < 0
ORDER BY score LIMIT 20;

-- Show both result sets
SELECT 'exhaustive-3term' as mode, * FROM ex_3term ORDER BY score LIMIT 5;
SELECT 'bmw-3term' as mode, * FROM bmw_3term ORDER BY score LIMIT 5;

-- TEST 5: Doc 8 (3-term doc) should be #1 in both
SELECT '3-term-8-is-top' as test,
    CASE WHEN (SELECT id FROM ex_3term ORDER BY score LIMIT 1) = 8
         AND (SELECT id FROM bmw_3term ORDER BY score LIMIT 1) = 8
    THEN 'PASS' ELSE 'FAIL' END as result;

-- TEST 6: 3-term results should match
SELECT '3-term-results-match' as test,
    CASE WHEN (SELECT COUNT(*) FROM
        (SELECT id FROM ex_3term EXCEPT SELECT id FROM bmw_3term) x) = 0
    THEN 'PASS' ELSE 'FAIL - 3-TERM RESULTS DIFFER' END as result;

DROP TABLE ex_3term, bmw_3term;
DROP TABLE three_term;

DROP EXTENSION pg_textsearch CASCADE;
