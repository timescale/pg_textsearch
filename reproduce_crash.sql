-- Self-contained script to reproduce the segfault
-- Run with: psql -f reproduce_crash.sql

CREATE DATABASE test_crash;
\c test_crash

CREATE EXTENSION pgtextsearch;

-- Create test table with documents
CREATE TABLE documents (
    doc_id serial PRIMARY KEY,
    content text
);

-- Insert test data
INSERT INTO documents (content) VALUES
    ('experimental investigation of the aerodynamics'),
    ('simple shear flow past a flat plate'),
    ('the boundary layer in simple shear flow'),
    ('approximate solutions of incompressible laminar'),
    ('one-dimensional transient heat conduction'),
    ('one-dimensional transient heat flow in multilayer'),
    ('the effect of controlled three-dimensional roughness'),
    ('measurements of the effect of two-dimensional'),
    ('transition studies and skin friction measurements'),
    ('the theory of the impact tube at low pressure');

-- Create BM25 index
CREATE INDEX doc_idx ON documents USING bm25(content)
    WITH (text_config='english');

-- This CTE query pattern causes the segfault
WITH ranked_results AS (
    SELECT
        doc_id,
        ROUND((content <@> to_bm25query('aerodynamic flow analysis', 'doc_idx'))::numeric, 4) as score,
        ROW_NUMBER() OVER (ORDER BY content <@> to_bm25query('aerodynamic flow analysis', 'doc_idx') ASC) as rank
    FROM documents
)
SELECT
    doc_id,
    rank,
    score
FROM ranked_results
WHERE rank <= 10
ORDER BY rank;

-- If we get here, the crash didn't occur
SELECT 'No crash occurred' as result;
