-- Test aerodocs with substantial Cranfield dataset to verify index usage
-- This test uses real Cranfield data to demonstrate index usage with realistic data size

CREATE EXTENSION IF NOT EXISTS tapir;

-- Load the full Cranfield dataset
\i benchmarks/sql/cranfield/01-load.sql
\i benchmarks/sql/cranfield/dataset.sql

\echo 'Full Cranfield dataset loaded. Testing index usage...'

-- Create tapir index
CREATE INDEX cranfield_tapir_idx ON cranfield_full_documents USING tapir(content)
    WITH (text_config='english', k1=1.2, b=0.75);

ANALYZE cranfield_full_documents;

-- Test that index is used for ORDER BY operations (the main issue we were debugging)
\echo 'Testing ORDER BY index usage with EXPLAIN:'

EXPLAIN (ANALYZE, BUFFERS)
SELECT doc_id, title, content <@> to_tpquery('aerodynamic flow boundary layer') as score
FROM cranfield_full_documents
ORDER BY content <@> to_tpquery('aerodynamic flow boundary layer') ASC
LIMIT 10;

\echo 'Test completed. The query plan above should show Index Scan, not Sort.'

-- Clean up
DROP INDEX cranfield_tapir_idx;
DROP TABLE cranfield_full_documents CASCADE;
