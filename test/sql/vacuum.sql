-- Test VACUUM behavior with BM25 indexes

-- Ensure extension is loaded
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test table
CREATE TABLE vacuum_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO vacuum_test (content) VALUES
    ('document one with test'),
    ('document two without keyword'),
    ('document three with test word'),
    ('document four plain text'),
    ('document five has test');

-- Create index
CREATE INDEX vacuum_idx ON vacuum_test
USING bm25 (content)
WITH (text_config = 'english');

-- Check index statistics before deletions
-- Extract just the total_docs line from debug output
SELECT split_part(
    split_part(bm25_dump_index('vacuum_idx')::text, 'total_docs: ', 2),
    E'\n', 1
) AS total_docs_before;

-- Delete some documents
DELETE FROM vacuum_test WHERE id IN (
    SELECT id FROM vacuum_test WHERE content LIKE '%test%' LIMIT 2
);

-- Check that index statistics haven't changed yet (deletions not processed)
SELECT split_part(
    split_part(bm25_dump_index('vacuum_idx')::text, 'total_docs: ', 2),
    E'\n', 1
) AS total_docs_after_delete;

-- Run VACUUM to clean up deleted tuples
VACUUM vacuum_test;

-- Check index statistics after VACUUM
-- Note: Our current implementation may not update stats until rebuild
SELECT split_part(
    split_part(bm25_dump_index('vacuum_idx')::text, 'total_docs: ', 2),
    E'\n', 1
) AS total_docs_after_vacuum;

-- Search should still work correctly after VACUUM
SELECT id, substring(content, 1, 30) as content_preview
FROM vacuum_test
WHERE content <@> to_bm25query('test', 'vacuum_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'vacuum_idx')
LIMIT 5;

-- Test VACUUM FULL (more aggressive cleanup)
-- VACUUM FULL rebuilds indexes
DELETE FROM vacuum_test WHERE content NOT LIKE '%test%';
-- We use \set VERBOSITY terse to avoid OID-specific error messages
\set VERBOSITY terse
VACUUM FULL vacuum_test;
\set VERBOSITY default

-- Check statistics after VACUUM FULL
SELECT split_part(
    split_part(bm25_dump_index('vacuum_idx')::text, 'total_docs: ', 2),
    E'\n', 1
) AS total_docs_after_vacuum_full;

-- Verify search still works
SELECT id, substring(content, 1, 30) as content_preview
FROM vacuum_test
WHERE content <@> to_bm25query('test', 'vacuum_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'vacuum_idx');

-- Clean up
DROP TABLE vacuum_test;
DROP EXTENSION pg_textsearch;
