-- Test case: segment_integrity
-- Verifies that segment operations preserve exact document scores.
--
-- Key insight: BM25 scores legitimately change when documents are added
-- (IDF and avgdl change). This test captures scores IMMEDIATELY before
-- an operation and verifies they are IDENTICAL after. This catches:
-- - Data loss during spill/merge
-- - Corruption of posting lists
-- - Document length encoding errors
-- - Term frequency encoding errors

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET pg_textsearch.log_scores = false;
SET enable_seqscan = off;

-- Create table with enough documents to be meaningful
CREATE TABLE integrity_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE INDEX integrity_test_idx ON integrity_test USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Insert a variety of documents with different term frequencies and lengths
INSERT INTO integrity_test (content) VALUES
    ('hello world'),
    ('hello hello world'),
    ('hello hello hello world world'),
    ('goodbye cruel world'),
    ('goodbye goodbye'),
    ('the quick brown fox jumps over the lazy dog'),
    ('fox fox fox hunting'),
    ('lazy dog sleeps'),
    ('world peace and harmony'),
    ('database indexing and search'),
    ('full text search with ranking'),
    ('search search search query'),
    ('query optimization techniques'),
    ('hello world database search'),
    ('unique document here'),
    ('another unique entry'),
    ('fox and dog are friends'),
    ('peace in the world'),
    ('cruel cruel world'),
    ('harmony and peace forever');

--------------------------------------------------------------------------------
-- Test 1: Spill integrity (memtable -> segment)
--------------------------------------------------------------------------------

-- Capture scores IMMEDIATELY before spill
CREATE TEMP TABLE pre_spill_scores AS
SELECT
    id,
    content,
    query,
    score
FROM (
    SELECT id, content, 'hello'::text AS query,
           (content <@> to_bm25query('hello', 'integrity_test_idx'))::numeric AS score
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'world'::text,
           (content <@> to_bm25query('world', 'integrity_test_idx'))::numeric
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'fox'::text,
           (content <@> to_bm25query('fox', 'integrity_test_idx'))::numeric
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'search'::text,
           (content <@> to_bm25query('search', 'integrity_test_idx'))::numeric
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'hello world'::text,
           (content <@> to_bm25query('hello world', 'integrity_test_idx'))::numeric
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'peace harmony'::text,
           (content <@> to_bm25query('peace harmony', 'integrity_test_idx'))::numeric
    FROM integrity_test
) sub;

SELECT 'Test 1: Spill integrity' AS test,
       COUNT(DISTINCT id) AS docs,
       COUNT(DISTINCT query) AS queries
FROM pre_spill_scores;

-- Spill to segment
SELECT bm25_spill_index('integrity_test_idx') IS NOT NULL AS spill_ok;

-- Verify ALL scores match after spill
SELECT
    CASE
        WHEN COUNT(*) = (SELECT COUNT(*) FROM pre_spill_scores)
        THEN 'PASS'
        ELSE 'FAIL'
    END AS spill_integrity,
    COUNT(*) AS matching,
    (SELECT COUNT(*) FROM pre_spill_scores) AS expected
FROM pre_spill_scores p
WHERE ABS(p.score -
          (p.content <@> to_bm25query(p.query, 'integrity_test_idx'))::numeric)
      <= 0.0001;

-- Show any mismatches (should be empty)
SELECT 'Spill mismatches:' AS label;
SELECT p.id, p.query,
       ROUND(p.score, 6) AS before,
       ROUND((p.content <@> to_bm25query(p.query, 'integrity_test_idx'))::numeric, 6)
           AS after
FROM pre_spill_scores p
WHERE ABS(p.score -
          (p.content <@> to_bm25query(p.query, 'integrity_test_idx'))::numeric)
      > 0.0001
LIMIT 5;

DROP TABLE pre_spill_scores;

--------------------------------------------------------------------------------
-- Test 2: Second spill integrity (add docs, then spill again)
--------------------------------------------------------------------------------

-- Add more documents
INSERT INTO integrity_test (content) VALUES
    ('new document after first spill'),
    ('hello from the other side'),
    ('world domination plans'),
    ('search and rescue mission'),
    ('fox news network');

-- Capture scores IMMEDIATELY before second spill
CREATE TEMP TABLE pre_spill2_scores AS
SELECT
    id,
    content,
    query,
    score
FROM (
    SELECT id, content, 'hello'::text AS query,
           (content <@> to_bm25query('hello', 'integrity_test_idx'))::numeric AS score
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'world'::text,
           (content <@> to_bm25query('world', 'integrity_test_idx'))::numeric
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'search'::text,
           (content <@> to_bm25query('search', 'integrity_test_idx'))::numeric
    FROM integrity_test
) sub;

SELECT 'Test 2: Second spill integrity' AS test,
       COUNT(DISTINCT id) AS docs
FROM pre_spill2_scores;

-- Second spill
SELECT bm25_spill_index('integrity_test_idx') IS NOT NULL AS spill2_ok;

-- Verify scores match
SELECT
    CASE
        WHEN COUNT(*) = (SELECT COUNT(*) FROM pre_spill2_scores)
        THEN 'PASS'
        ELSE 'FAIL'
    END AS spill2_integrity,
    COUNT(*) AS matching,
    (SELECT COUNT(*) FROM pre_spill2_scores) AS expected
FROM pre_spill2_scores p
WHERE ABS(p.score -
          (p.content <@> to_bm25query(p.query, 'integrity_test_idx'))::numeric)
      <= 0.0001;

DROP TABLE pre_spill2_scores;

--------------------------------------------------------------------------------
-- Test 3: Merge integrity
--------------------------------------------------------------------------------

-- Configure for merge (2 segments triggers merge)
SET pg_textsearch.segments_per_level = 2;

-- Add more docs and spill to trigger merge
INSERT INTO integrity_test (content) VALUES
    ('merge test document one'),
    ('merge test hello world'),
    ('merge fox and dog'),
    ('search merge optimization');

-- Capture scores IMMEDIATELY before merge-triggering spill
CREATE TEMP TABLE pre_merge_scores AS
SELECT
    id,
    content,
    query,
    score
FROM (
    SELECT id, content, 'hello'::text AS query,
           (content <@> to_bm25query('hello', 'integrity_test_idx'))::numeric AS score
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'world'::text,
           (content <@> to_bm25query('world', 'integrity_test_idx'))::numeric
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'merge'::text,
           (content <@> to_bm25query('merge', 'integrity_test_idx'))::numeric
    FROM integrity_test
    UNION ALL
    SELECT id, content, 'search'::text,
           (content <@> to_bm25query('search', 'integrity_test_idx'))::numeric
    FROM integrity_test
) sub;

SELECT 'Test 3: Merge integrity' AS test,
       COUNT(DISTINCT id) AS docs
FROM pre_merge_scores;

-- This spill triggers merge
SELECT bm25_spill_index('integrity_test_idx') IS NOT NULL AS merge_spill_ok;

-- Verify all scores match after merge
SELECT
    CASE
        WHEN COUNT(*) = (SELECT COUNT(*) FROM pre_merge_scores)
        THEN 'PASS'
        ELSE 'FAIL'
    END AS merge_integrity,
    COUNT(*) AS matching,
    (SELECT COUNT(*) FROM pre_merge_scores) AS expected
FROM pre_merge_scores p
WHERE ABS(p.score -
          (p.content <@> to_bm25query(p.query, 'integrity_test_idx'))::numeric)
      <= 0.0001;

-- Show any mismatches (should be empty)
SELECT 'Merge mismatches:' AS label;
SELECT p.id, p.query,
       ROUND(p.score, 6) AS before,
       ROUND((p.content <@> to_bm25query(p.query, 'integrity_test_idx'))::numeric, 6)
           AS after
FROM pre_merge_scores p
WHERE ABS(p.score -
          (p.content <@> to_bm25query(p.query, 'integrity_test_idx'))::numeric)
      > 0.0001
LIMIT 5;

DROP TABLE pre_merge_scores;

--------------------------------------------------------------------------------
-- Test 4: Document reachability
-- Verify documents with matching terms are returned by index scan.
-- Note: Documents without any queried terms correctly return score=0
-- and may not appear in ORDER BY results - this is expected behavior.
--------------------------------------------------------------------------------

SELECT 'Test 4: Document reachability' AS test;

-- Test that documents with 'hello' are found when searching for 'hello'
-- Count docs containing 'hello' in table vs docs returned by index
SELECT
    'hello' AS term,
    (SELECT COUNT(*) FROM integrity_test
     WHERE to_tsvector('english', content) @@ to_tsquery('english', 'hello'))
        AS expected,
    (SELECT COUNT(*) FROM (
        SELECT id FROM integrity_test
        WHERE (content <@> to_bm25query('hello', 'integrity_test_idx')) < 0
    ) t) AS found,
    CASE
        WHEN (SELECT COUNT(*) FROM integrity_test
              WHERE to_tsvector('english', content) @@ to_tsquery('english', 'hello'))
           = (SELECT COUNT(*) FROM (
                SELECT id FROM integrity_test
                WHERE (content <@> to_bm25query('hello', 'integrity_test_idx')) < 0
              ) t)
        THEN 'PASS'
        ELSE 'FAIL'
    END AS hello_reachable;

SELECT
    'world' AS term,
    (SELECT COUNT(*) FROM integrity_test
     WHERE to_tsvector('english', content) @@ to_tsquery('english', 'world'))
        AS expected,
    (SELECT COUNT(*) FROM (
        SELECT id FROM integrity_test
        WHERE (content <@> to_bm25query('world', 'integrity_test_idx')) < 0
    ) t) AS found,
    CASE
        WHEN (SELECT COUNT(*) FROM integrity_test
              WHERE to_tsvector('english', content) @@ to_tsquery('english', 'world'))
           = (SELECT COUNT(*) FROM (
                SELECT id FROM integrity_test
                WHERE (content <@> to_bm25query('world', 'integrity_test_idx')) < 0
              ) t)
        THEN 'PASS'
        ELSE 'FAIL'
    END AS world_reachable;

SELECT
    'search' AS term,
    (SELECT COUNT(*) FROM integrity_test
     WHERE to_tsvector('english', content) @@ to_tsquery('english', 'search'))
        AS expected,
    (SELECT COUNT(*) FROM (
        SELECT id FROM integrity_test
        WHERE (content <@> to_bm25query('search', 'integrity_test_idx')) < 0
    ) t) AS found,
    CASE
        WHEN (SELECT COUNT(*) FROM integrity_test
              WHERE to_tsvector('english', content) @@ to_tsquery('english', 'search'))
           = (SELECT COUNT(*) FROM (
                SELECT id FROM integrity_test
                WHERE (content <@> to_bm25query('search', 'integrity_test_idx')) < 0
              ) t)
        THEN 'PASS'
        ELSE 'FAIL'
    END AS search_reachable;

--------------------------------------------------------------------------------
-- Summary
--------------------------------------------------------------------------------

SELECT 'SUMMARY' AS section,
       COUNT(*) AS total_documents
FROM integrity_test;

-- Cleanup
DROP TABLE integrity_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
