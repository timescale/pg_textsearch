-- Test memory limit enforcement for pg_textsearch indexes

-- Create extension if not exists
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test table
DROP TABLE IF EXISTS memory_test CASCADE;
CREATE TABLE memory_test (id serial PRIMARY KEY, content text);

-- Set a very low memory limit (1MB) to trigger the limit
SET pg_textsearch.index_memory_limit = 1;

-- Create an index with the low memory limit
CREATE INDEX idx_memory_test
ON memory_test
USING bm25(content)
WITH (text_config='english');

-- Insert some documents
INSERT INTO memory_test (content)
VALUES ('This is a test document with some content');

INSERT INTO memory_test (content)
VALUES ('Another document with different words');

-- Try to insert many documents with lots of unique terms to trigger flush
-- With flush enabled, this should succeed but trigger flushes
-- Use fewer documents to keep test time reasonable
BEGIN;
DO $$
DECLARE
    i integer;
    doc text;
BEGIN
    FOR i IN 1..50 LOOP
        -- Generate document with unique terms to increase memory usage
        doc := 'Document number ' || i || ' with unique terms: ';
        -- Add lots of unique words per document
        FOR j IN 1..100 LOOP
            doc := doc || ' term_' || i || '_' || j;
        END LOOP;

        INSERT INTO memory_test (content) VALUES (doc);
    END LOOP;
END $$;
COMMIT;

-- Verify that the index still works after flush
SELECT id, content
FROM memory_test
WHERE content <@> to_bm25query('test', 'idx_memory_test') < -0.001
ORDER BY content <@> to_bm25query('test', 'idx_memory_test')
LIMIT 5;

-- Reset to default memory limit
RESET pg_textsearch.index_memory_limit;

-- Clean up
DROP TABLE memory_test CASCADE;
