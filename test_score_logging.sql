-- Test score logging GUC to verify ORDER BY and <@> operator paths produce same scores
DROP EXTENSION IF EXISTS tapir CASCADE;
CREATE EXTENSION tapir;

-- Create test table
CREATE TABLE test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test data
INSERT INTO test_docs (content) VALUES
    ('database management system'),
    ('postgresql database server'),
    ('information retrieval system'),
    ('database query optimization'),
    ('full text search database');

-- Create tapir index
CREATE INDEX test_tapir_idx ON test_docs USING tapir(content) WITH (text_config='english');

-- Enable score logging
SET tapir.log_scores = true;

\echo '=== Testing ORDER BY path (index scan) ==='
-- This uses the index scan path with ORDER BY
SELECT id, content
FROM test_docs
ORDER BY content <@> to_tpquery('database system', 'test_tapir_idx') ASC
LIMIT 3;

\echo '=== Testing <@> operator path (standalone scoring) ==='
-- This computes scores directly using the operator
SELECT id, content, content <@> to_tpquery('database system', 'test_tapir_idx') as score
FROM test_docs
ORDER BY score ASC
LIMIT 3;

-- Disable score logging
SET tapir.log_scores = false;

-- Clean up
DROP TABLE test_docs CASCADE;
