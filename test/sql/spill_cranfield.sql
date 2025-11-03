-- Comprehensive spill test using Cranfield dataset
-- Tests that scores remain consistent across memtable and segment storage

CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET pg_textsearch.log_scores = true;
SET enable_seqscan = off;

-- Create Cranfield table with full documents
DROP TABLE IF EXISTS cranfield_spill CASCADE;
CREATE TABLE cranfield_spill (
    doc_id INTEGER PRIMARY KEY,
    title TEXT,
    author TEXT,
    bibliography TEXT,
    content TEXT,
    full_text TEXT GENERATED ALWAYS AS (
        COALESCE(title, '') || ' ' ||
        COALESCE(author, '') || ' ' ||
        COALESCE(content, '')
    ) STORED
);

-- Load subset of Cranfield data (first 20 docs for manageable test)
INSERT INTO cranfield_spill
SELECT doc_id, title, author, bibliography, content
FROM cranfield_full_documents
WHERE doc_id <= 20;

-- Create index
CREATE INDEX cranfield_spill_idx ON cranfield_spill
USING bm25(full_text) WITH (text_config='english');

-- Test Query 1: Classic aerodynamics query
-- Save scores BEFORE spill
CREATE TEMP TABLE scores_before_spill AS
SELECT
    doc_id,
    title,
    ROUND((full_text <@> to_bm25query('boundary layer flow', 'cranfield_spill_idx'))::numeric, 6) as score
FROM cranfield_spill
WHERE full_text <@> to_bm25query('boundary layer flow', 'cranfield_spill_idx') < 0
ORDER BY score;

-- Display scores before spill
SELECT * FROM scores_before_spill ORDER BY score LIMIT 10;

-- Count documents in memtable before spill
SELECT COUNT(*) as docs_in_memtable FROM cranfield_spill;

-- SPILL 1: Force first memtable spill
SELECT tp_spill_memtable('cranfield_spill_idx');

-- Test scores immediately after first spill
CREATE TEMP TABLE scores_after_spill1 AS
SELECT
    doc_id,
    title,
    ROUND((full_text <@> to_bm25query('boundary layer flow', 'cranfield_spill_idx'))::numeric, 6) as score
FROM cranfield_spill
WHERE full_text <@> to_bm25query('boundary layer flow', 'cranfield_spill_idx') < 0
ORDER BY score;

-- Verify scores match after first spill
SELECT
    'Spill 1 Score Check' as test,
    COUNT(*) as mismatches,
    CASE WHEN COUNT(*) = 0 THEN 'PASS' ELSE 'FAIL' END as result
FROM scores_before_spill b
FULL OUTER JOIN scores_after_spill1 a USING (doc_id)
WHERE b.score IS DISTINCT FROM a.score OR b.doc_id IS NULL OR a.doc_id IS NULL;

-- Add more documents to memtable
INSERT INTO cranfield_spill
SELECT doc_id, title, author, bibliography, content
FROM cranfield_full_documents
WHERE doc_id BETWEEN 21 AND 40;

-- Get scores with mixed memtable + segment
CREATE TEMP TABLE scores_mixed AS
SELECT
    doc_id,
    ROUND((full_text <@> to_bm25query('boundary layer flow', 'cranfield_spill_idx'))::numeric, 6) as score
FROM cranfield_spill
WHERE full_text <@> to_bm25query('boundary layer flow', 'cranfield_spill_idx') < 0
ORDER BY score;

-- Display top results from mixed storage
SELECT doc_id, LEFT(title, 50) as title_preview, score
FROM cranfield_spill
JOIN scores_mixed USING (doc_id)
ORDER BY score
LIMIT 10;

-- SPILL 2: Force second memtable spill (should chain segments)
SELECT tp_spill_memtable('cranfield_spill_idx');

-- Test scores after second spill (now everything in segments)
CREATE TEMP TABLE scores_after_spill2 AS
SELECT
    doc_id,
    ROUND((full_text <@> to_bm25query('boundary layer flow', 'cranfield_spill_idx'))::numeric, 6) as score
FROM cranfield_spill
WHERE full_text <@> to_bm25query('boundary layer flow', 'cranfield_spill_idx') < 0
ORDER BY score;

-- Verify scores still match with multiple segments
SELECT
    'Spill 2 Score Check' as test,
    COUNT(*) as mismatches,
    CASE WHEN COUNT(*) = 0 THEN 'PASS' ELSE 'FAIL' END as result
FROM scores_mixed m
FULL OUTER JOIN scores_after_spill2 a USING (doc_id)
WHERE m.score IS DISTINCT FROM a.score OR m.doc_id IS NULL OR a.doc_id IS NULL;

-- Add final batch of documents
INSERT INTO cranfield_spill
SELECT doc_id, title, author, bibliography, content
FROM cranfield_full_documents
WHERE doc_id BETWEEN 41 AND 74;

-- SPILL 3: Third spill to test segment chain further
SELECT tp_spill_memtable('cranfield_spill_idx');

-- Final comprehensive test with all 74 documents
CREATE TEMP TABLE scores_final AS
SELECT
    doc_id,
    ROUND((full_text <@> to_bm25query('aerodynamic pressure distribution', 'cranfield_spill_idx'))::numeric, 6) as score
FROM cranfield_spill
WHERE full_text <@> to_bm25query('aerodynamic pressure distribution', 'cranfield_spill_idx') < 0
ORDER BY score;

-- Show top 10 results
SELECT
    doc_id,
    LEFT(title, 60) as title_preview,
    score
FROM cranfield_spill
JOIN scores_final USING (doc_id)
ORDER BY score
LIMIT 10;

-- Test different query to ensure segment lookup works
SELECT
    COUNT(*) as matching_docs,
    MIN(score) as best_score,
    MAX(score) as worst_score,
    AVG(score) as avg_score
FROM (
    SELECT
        doc_id,
        (full_text <@> to_bm25query('supersonic flow shock waves', 'cranfield_spill_idx'))::numeric as score
    FROM cranfield_spill
    WHERE full_text <@> to_bm25query('supersonic flow shock waves', 'cranfield_spill_idx') < 0
) t;

-- Summary statistics
SELECT
    'Test Summary' as info,
    (SELECT COUNT(*) FROM cranfield_spill) as total_docs,
    (SELECT COUNT(DISTINCT doc_id) FROM scores_final) as docs_with_scores,
    (SELECT MIN(score) FROM scores_final) as best_score,
    (SELECT COUNT(*) FROM scores_final WHERE score < -1) as high_relevance_docs;

-- Cleanup
DROP TABLE scores_before_spill;
DROP TABLE scores_after_spill1;
DROP TABLE scores_after_spill2;
DROP TABLE scores_mixed;
DROP TABLE scores_final;
DROP TABLE cranfield_spill CASCADE;
-- Keep cranfield_full_documents for future tests
