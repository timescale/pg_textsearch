-- Test template for BM25 validation system
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test table
CREATE TABLE validation_docs (
    id integer PRIMARY KEY,
    content text
);

-- Insert test documents
INSERT INTO validation_docs VALUES
    (1, 'hello world example'),
    (2, 'goodbye cruel world'),
    (3, 'hello hello hello'),
    (4, 'random document text');

-- Create BM25 index
CREATE INDEX validation_idx ON validation_docs
USING bm25 (content)
WITH (text_config='english');

-- VALIDATE_BM25: table=validation_docs query="hello" index=validation_idx
SELECT id, content,
       ROUND((content <@> to_bm25query('hello', 'validation_idx'))::numeric, 6) as score
FROM validation_docs
WHERE content <@> to_bm25query('hello', 'validation_idx') < 0
ORDER BY score DESC;

-- VALIDATE_BM25: table=validation_docs query="world" index=validation_idx
SELECT id, content,
       ROUND((content <@> to_bm25query('world', 'validation_idx'))::numeric, 6) as score
FROM validation_docs
ORDER BY score DESC;

-- Cleanup
DROP TABLE validation_docs CASCADE;
