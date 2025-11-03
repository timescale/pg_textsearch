-- Simple test to verify segment spilling works and scores remain consistent

CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET pg_textsearch.log_scores = true;
SET enable_seqscan = off;

-- Create test table with aerodynamics documents
DROP TABLE IF EXISTS simple_spill CASCADE;
CREATE TABLE simple_spill (
    doc_id INTEGER PRIMARY KEY,
    content TEXT
);

-- Insert 20 documents
INSERT INTO simple_spill VALUES
(1, 'boundary layer flow over flat plate with heat transfer'),
(2, 'turbulent boundary layer characteristics in supersonic flow'),
(3, 'shock wave interactions with boundary layer separation'),
(4, 'laminar boundary layer transition on swept wings'),
(5, 'boundary layer control using suction methods'),
(6, 'hypersonic boundary layer stability analysis'),
(7, 'flow separation in adverse pressure gradients'),
(8, 'boundary layer measurement techniques using hot wire'),
(9, 'compressible boundary layer theory and experiments'),
(10, 'boundary layer effects on aerodynamic coefficients'),
(11, 'unsteady boundary layer behavior in oscillating flow'),
(12, 'three dimensional boundary layer on rotating disk'),
(13, 'boundary layer interaction with shock waves'),
(14, 'thermal boundary layer in high temperature flow'),
(15, 'boundary layer transition prediction methods'),
(16, 'viscous effects in thin boundary layers'),
(17, 'boundary layer separation and reattachment phenomena'),
(18, 'numerical simulation of boundary layer flows'),
(19, 'experimental study of boundary layer instabilities'),
(20, 'boundary layer growth on curved surfaces');

-- Create index
CREATE INDEX simple_spill_idx ON simple_spill
USING bm25(content) WITH (text_config='english');

-- Test Query: Get scores BEFORE spill
CREATE TEMP TABLE scores_before AS
SELECT
    doc_id,
    content,
    ROUND((content <@> to_bm25query('boundary layer flow', 'simple_spill_idx'))::numeric, 6) as score
FROM simple_spill
WHERE content <@> to_bm25query('boundary layer flow', 'simple_spill_idx') < 0
ORDER BY score;

-- Display scores before spill
SELECT doc_id, LEFT(content, 50) as content_preview, score
FROM scores_before
ORDER BY score
LIMIT 10;

-- Force memtable spill
SELECT tp_spill_memtable('simple_spill_idx');

-- Test scores immediately after spill
CREATE TEMP TABLE scores_after AS
SELECT
    doc_id,
    content,
    ROUND((content <@> to_bm25query('boundary layer flow', 'simple_spill_idx'))::numeric, 6) as score
FROM simple_spill
WHERE content <@> to_bm25query('boundary layer flow', 'simple_spill_idx') < 0
ORDER BY score;

-- Verify scores match
SELECT
    'Score Consistency Check' as test,
    COUNT(*) as mismatches,
    CASE WHEN COUNT(*) = 0 THEN 'PASS' ELSE 'FAIL' END as result
FROM scores_before b
FULL OUTER JOIN scores_after a USING (doc_id)
WHERE b.score IS DISTINCT FROM a.score OR b.doc_id IS NULL OR a.doc_id IS NULL;

-- Add more documents to memtable
INSERT INTO simple_spill VALUES
(21, 'new document about flow visualization techniques'),
(22, 'study of boundary conditions in fluid dynamics'),
(23, 'experimental boundary layer measurements'),
(24, 'computational fluid dynamics boundary simulations'),
(25, 'boundary layer theory applications');

-- Test with mixed memtable + segment
CREATE TEMP TABLE scores_mixed AS
SELECT
    doc_id,
    ROUND((content <@> to_bm25query('boundary layer flow', 'simple_spill_idx'))::numeric, 6) as score
FROM simple_spill
WHERE content <@> to_bm25query('boundary layer flow', 'simple_spill_idx') < 0
ORDER BY score;

-- Display top results from mixed storage
SELECT doc_id, LEFT(content, 50) as content_preview, score
FROM simple_spill
JOIN scores_mixed USING (doc_id)
ORDER BY score
LIMIT 10;

-- Second spill
SELECT tp_spill_memtable('simple_spill_idx');

-- Final consistency check
CREATE TEMP TABLE scores_final AS
SELECT
    doc_id,
    ROUND((content <@> to_bm25query('boundary layer flow', 'simple_spill_idx'))::numeric, 6) as score
FROM simple_spill
WHERE content <@> to_bm25query('boundary layer flow', 'simple_spill_idx') < 0
ORDER BY score;

-- Verify scores still consistent
SELECT
    'Final Score Check' as test,
    COUNT(*) as mismatches,
    CASE WHEN COUNT(*) = 0 THEN 'PASS' ELSE 'FAIL' END as result
FROM scores_mixed m
FULL OUTER JOIN scores_final f USING (doc_id)
WHERE m.score IS DISTINCT FROM f.score OR m.doc_id IS NULL OR f.doc_id IS NULL;

-- Summary
SELECT
    'Test Summary' as info,
    (SELECT COUNT(*) FROM simple_spill) as total_docs,
    (SELECT COUNT(DISTINCT doc_id) FROM scores_final) as docs_with_scores,
    (SELECT MIN(score) FROM scores_final) as best_score;

-- Cleanup
DROP TABLE scores_before;
DROP TABLE scores_after;
DROP TABLE scores_mixed;
DROP TABLE scores_final;
DROP TABLE simple_spill CASCADE;
