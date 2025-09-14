-- Aerodynamics Documents tapir Test - Sample Dataset for Development/Debugging
-- This test validates tapir implementation using a small sample of aerodynamics documents
-- Contains 5 sample aerodynamics documents with 3 queries for quick testing
-- Tests both dataset loading and tapir scoring functionality with the <@> operator

-- Load tapir extension
CREATE EXTENSION IF NOT EXISTS tapir;

\set ON_ERROR_STOP on
\echo 'Testing tapir with sample aerodynamics documents...'

-- Create tables for aerodynamics documents
CREATE TABLE aerodocs_documents (
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

CREATE TABLE cranfield_queries (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);

CREATE TABLE cranfield_expected_rankings (
    query_id INTEGER NOT NULL,
    doc_id INTEGER NOT NULL,
    rank INTEGER NOT NULL,
    tapir_score FLOAT NOT NULL,
    PRIMARY KEY (query_id, doc_id)
);

\echo 'Creating sample Cranfield-style dataset for testing...'

-- Insert sample documents for testing
INSERT INTO aerodocs_documents (doc_id, title, author, content) VALUES
(1, 'Aerodynamic Flow Over Wings', 'Smith, J.', 'aerodynamic flow boundary layer wing design'),
(2, 'Turbulent Boundary Layer Analysis', 'Jones, M.', 'boundary layer turbulent flow analysis'),
(3, 'Supersonic Aircraft Design', 'Brown, K.', 'supersonic aircraft design aerodynamic flow'),
(4, 'Wing Design Optimization', 'Davis, R.', 'wing design optimization aerodynamic performance'),
(5, 'Computational Fluid Dynamics', 'Wilson, T.', 'computational fluid dynamics boundary layer');

-- Insert sample queries
INSERT INTO cranfield_queries (query_id, query_text) VALUES
(1, 'aerodynamic flow analysis'),
(2, 'boundary layer turbulent'),
(3, 'wing design optimization');

-- Insert sample expected rankings (for testing purposes)
INSERT INTO cranfield_expected_rankings (query_id, doc_id, rank, tapir_score) VALUES
(1, 1, 1, 1.5),
(1, 3, 2, 1.2),
(1, 4, 3, 1.0);

-- Create tapir index for Cranfield documents with text_config
CREATE INDEX cranfield_tapir_idx ON aerodocs_documents USING tapir(full_text)
    WITH (text_config='english', k1=1.2, b=0.75);

\echo 'tapir index created. Running validation tests...'

-- Disable sequential scans to ensure index usage
SET enable_seqscan = off;

-- Test 1: Basic search functionality
\echo 'Test 1: Basic search with <@> operator'
SELECT
    doc_id,
    LEFT(title, 60) as title_preview,
    ROUND((full_text <@> 'aerodynamic flow'::tpquery)::numeric, 4) as score
FROM aerodocs_documents
ORDER BY 3 DESC
LIMIT 5;

-- Test 2: Multi-term search
\echo 'Test 2: Multi-term search'
SELECT
    doc_id,
    LEFT(title, 60) as title_preview,
    ROUND((full_text <@> 'boundary layer turbulent'::tpquery)::numeric, 4) as score
FROM aerodocs_documents
ORDER BY 3 DESC
LIMIT 5;

-- Test 3: Validation against expected rankings for Query 1
\echo 'Test 3: Validation against Cranfield reference results'
WITH tapir_results AS (
    SELECT
        doc_id,
        ROUND((full_text <@> to_tpquery((SELECT query_text FROM cranfield_queries WHERE query_id = 1)))::numeric, 4) as tapir_score,
        ROW_NUMBER() OVER (ORDER BY full_text <@> to_tpquery((SELECT query_text FROM cranfield_queries WHERE query_id = 1)) DESC) as tapir_rank
    FROM aerodocs_documents
),
reference_results AS (
    SELECT doc_id, rank as expected_rank, tapir_score as expected_score
    FROM cranfield_expected_rankings
    WHERE query_id = 1 AND rank <= 10
)
SELECT
    q.query_text,
    br.doc_id,
    br.tapir_rank,
    rr.expected_rank,
    ROUND(br.tapir_score::numeric, 4) as actual_score,
    ROUND(rr.expected_score::numeric, 4) as expected_score,
    LEFT(d.title, 50) as title_preview
FROM cranfield_queries q
CROSS JOIN tapir_results br
LEFT JOIN reference_results rr ON br.doc_id = rr.doc_id
JOIN aerodocs_documents d ON br.doc_id = d.doc_id
WHERE q.query_id = 1 AND br.tapir_rank <= 10
ORDER BY br.tapir_rank;

-- Test 4: Performance test with multiple queries
\echo 'Test 4: Performance test with top-k search'
SELECT
    q.query_id,
    LEFT(q.query_text, 40) || '...' as query_preview,
    COUNT(d.doc_id) as total_docs,
    COUNT(CASE WHEN d.full_text <@> to_tpquery(q.query_text) < 0 THEN 1 END) as matching_docs
FROM cranfield_queries q
CROSS JOIN aerodocs_documents d
WHERE q.query_id <= 5  -- Test first 5 queries for performance
GROUP BY q.query_id, q.query_text
ORDER BY q.query_id;

-- Test 5: Index usage verification
\echo 'Test 5: Verify index usage with EXPLAIN'
SET jit = off;
EXPLAIN
SELECT doc_id, ROUND((full_text <@> 'supersonic aircraft design'::tpquery)::numeric, 4) as score
FROM aerodocs_documents
ORDER BY 2 DESC
LIMIT 10;

-- Reset settings
SET enable_seqscan = on;

\echo 'Cranfield tapir validation completed successfully.'
\echo 'All tests demonstrate proper tapir functionality with the <@> operator.'

-- Clean up
DROP TABLE aerodocs_documents CASCADE;
DROP TABLE cranfield_queries CASCADE;
DROP TABLE cranfield_expected_rankings CASCADE;
