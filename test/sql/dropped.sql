-- Test behavior when using a dropped index name in queries

-- Ensure extension is loaded
CREATE EXTENSION IF NOT EXISTS pgtextsearch;

-- Create test table
CREATE TABLE dropped_idx_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO dropped_idx_test (content) VALUES
    ('first test document'),
    ('second test document'),
    ('third test document'),
    ('fourth test document'),
    ('fifth test document');

-- Create index
CREATE INDEX dropped_idx ON dropped_idx_test
USING bm25 (content)
WITH (text_config = 'english');

-- Query should work with index
SELECT COUNT(*) AS with_index
FROM dropped_idx_test
WHERE content <@> to_bm25query('test', 'dropped_idx') < -0.001;

-- Drop the index
DROP INDEX dropped_idx;

-- Query should ERROR when index doesn't exist
SELECT COUNT(*) AS after_drop
FROM dropped_idx_test
WHERE content <@> to_bm25query('test', 'dropped_idx') < -0.001;

-- Also test with a completely non-existent index name
SELECT COUNT(*) AS nonexistent
FROM dropped_idx_test
WHERE content <@> to_bm25query('test', 'totally_fake_index') < -0.001;

-- Clean up
DROP TABLE dropped_idx_test;
DROP EXTENSION pgtextsearch;
