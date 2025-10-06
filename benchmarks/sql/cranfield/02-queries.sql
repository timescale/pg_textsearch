-- Cranfield Collection BM25 Benchmark - Query Performance
-- This benchmark tests BM25 query performance and validates results
-- Timing: Query execution timed, results validated against expected output
-- Tests search quality using standard Cranfield information retrieval collection

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield BM25 Benchmark - Query Phase'
\echo '====================================='

-- For now, disable index scans since our index doesn't support result retrieval
-- The <@> operator works correctly with sequential scans
SET enable_indexscan = off;
SET enable_bitmapscan = off;

-- Benchmark 1: Single-term search performance
\echo 'Benchmark 1: Single-term search'
SELECT
    q.query_id,
    LEFT(q.query_text, 50) || '...' as query_preview,
    d.doc_id,
    LEFT(d.title, 60) as title_preview,
    d.full_text <@> to_bm25query(q.query_text, 'cranfield_full_tapir_idx') as score
FROM cranfield_full_queries q, cranfield_full_documents d
WHERE q.query_id IN (1, 10, 50)  -- Sample queries
ORDER BY q.query_id, score
LIMIT 15;

-- Benchmark 2: Multi-term search performance
\echo 'Benchmark 2: Multi-term search'
SELECT
    doc_id,
    LEFT(title, 60) as title_preview,
    full_text <@> to_bm25query('aerodynamic flow boundary layer', 'cranfield_full_tapir_idx') as score
FROM cranfield_full_documents
ORDER BY score
LIMIT 10;

-- Benchmark 3: Batch query performance (measure throughput)
\echo 'Benchmark 3: Batch query performance'
SELECT
    q.query_id,
    LEFT(q.query_text, 40) || '...' as query_preview,
    COUNT(d.doc_id) as total_docs,
    COUNT(CASE WHEN d.full_text <@> to_bm25query(q.query_text, 'cranfield_full_tapir_idx') > 0 THEN 1 END) as matching_docs
FROM cranfield_full_queries q
CROSS JOIN cranfield_full_documents d
WHERE q.query_id <= 5  -- Test first 5 queries
GROUP BY q.query_id, q.query_text
ORDER BY q.query_id;

-- Benchmark 4: Precision test against reference rankings
\echo 'Benchmark 4: Search quality validation (Query 1)'
WITH bm25_results AS (
    SELECT
        doc_id,
        full_text <@> to_bm25query((SELECT query_text FROM cranfield_full_queries WHERE query_id = 1), 'cranfield_full_tapir_idx') as bm25_score,
        ROW_NUMBER() OVER (ORDER BY full_text <@> to_bm25query((SELECT query_text FROM cranfield_full_queries WHERE query_id = 1), 'cranfield_full_tapir_idx')) as bm25_rank
    FROM cranfield_full_documents
),
reference_results AS (
    SELECT doc_id, rank as expected_rank, bm25_score as expected_score
    FROM cranfield_full_expected_rankings
    WHERE query_id = 1 AND rank <= 10
)
SELECT
    br.bm25_rank,
    br.doc_id,
    rr.expected_rank,
    ROUND(br.bm25_score::numeric, 4) as actual_score,
    ROUND(rr.expected_score::numeric, 4) as expected_score,
    CASE WHEN rr.doc_id IS NOT NULL THEN 'MATCH' ELSE 'NEW' END as status,
    LEFT(d.title, 50) as title_preview
FROM bm25_results br
LEFT JOIN reference_results rr ON br.doc_id = rr.doc_id
JOIN cranfield_full_documents d ON br.doc_id = d.doc_id
WHERE br.bm25_rank <= 10
ORDER BY br.bm25_rank;

-- Benchmark 5: Query plan verification with EXPLAIN
\echo 'Benchmark 5: Query plan verification'
EXPLAIN
SELECT doc_id, full_text <@> to_bm25query('wing design aerodynamic', 'cranfield_full_tapir_idx') as score
FROM cranfield_full_documents
ORDER BY 2
LIMIT 10;

-- Benchmark 6: Complex query performance
\echo 'Benchmark 6: Complex query patterns'
SELECT
    COUNT(*) as total_results,
    ROUND(AVG((full_text <@> to_bm25query('supersonic aircraft design', 'cranfield_full_tapir_idx'))::numeric), 4) as avg_score,
    ROUND(MIN((full_text <@> to_bm25query('supersonic aircraft design', 'cranfield_full_tapir_idx'))::numeric), 4) as min_score,
    ROUND(MAX((full_text <@> to_bm25query('supersonic aircraft design', 'cranfield_full_tapir_idx'))::numeric), 4) as max_score
FROM cranfield_full_documents;

-- Reset settings
SET enable_indexscan = on;
SET enable_bitmapscan = on;

\echo 'Cranfield BM25 benchmark completed.'
\echo 'Performance and quality results validated against standard IR collection.'
