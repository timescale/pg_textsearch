-- Test handling of row deletions in indexed tables

-- Ensure extension is loaded
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test table
CREATE TABLE deletion_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert some documents
INSERT INTO deletion_test (content) VALUES
    ('first document with some content'),
    ('second document with different content'),
    ('third document with test keyword'),
    ('fourth document without the keyword'),
    ('fifth document with test again');

-- Create index
CREATE INDEX deletion_idx ON deletion_test
USING bm25 (content)
WITH (text_config = 'english');

-- Initial search - should find documents with "test"
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_bm25query('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'deletion_idx') DESC
LIMIT 5;

-- Delete one of the documents that contains "test"
DELETE FROM deletion_test WHERE id = 3;

-- Search again - deleted document should not appear
-- (Note: corpus statistics won't be updated, but shouldn't crash)
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_bm25query('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'deletion_idx') DESC
LIMIT 5;

-- Delete a document that doesn't contain "test"
DELETE FROM deletion_test WHERE id = 4;

-- Search again - should still work
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_bm25query('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'deletion_idx') DESC
LIMIT 5;

-- Delete all remaining documents with "test"
DELETE FROM deletion_test WHERE content LIKE '%test%';

-- Search again - should return no results (but not crash)
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_bm25query('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'deletion_idx') DESC
LIMIT 5;

-- Insert new documents after deletions
INSERT INTO deletion_test (content) VALUES
    ('new document with test keyword'),
    ('another new document');

-- Search again - should find the new document
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_bm25query('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'deletion_idx') DESC
LIMIT 5;

-- Test with empty table after deleting everything
DELETE FROM deletion_test;

-- Search in empty table - should return no results
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_bm25query('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'deletion_idx') DESC
LIMIT 5;

-- Test VACUUM behavior after deletions
-- First, repopulate the table
INSERT INTO deletion_test (content) VALUES
    ('document one with test'),
    ('document two without keyword'),
    ('document three with test word'),
    ('document four plain text'),
    ('document five has test');

-- Check index statistics before deletions
-- Extract just the total_docs line from debug output
SELECT split_part(
    split_part(bm25_debug_dump_index('deletion_idx')::text, 'total_docs: ', 2),
    E'\n', 1
) AS total_docs_before;

-- Delete some documents
DELETE FROM deletion_test WHERE id IN (
    SELECT id FROM deletion_test WHERE content LIKE '%test%' LIMIT 2
);

-- Check that index statistics haven't changed yet (deletions not processed)
SELECT split_part(
    split_part(bm25_debug_dump_index('deletion_idx')::text, 'total_docs: ', 2),
    E'\n', 1
) AS total_docs_after_delete;

-- Run VACUUM to clean up deleted tuples
VACUUM deletion_test;

-- Check index statistics after VACUUM
-- Note: Our current implementation may not update stats until rebuild
SELECT split_part(
    split_part(bm25_debug_dump_index('deletion_idx')::text, 'total_docs: ', 2),
    E'\n', 1
) AS total_docs_after_vacuum;

-- Search should still work correctly after VACUUM
SELECT id, substring(content, 1, 30) as content_preview
FROM deletion_test
WHERE content <@> to_bm25query('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'deletion_idx')
LIMIT 5;

-- Test VACUUM FULL (more aggressive cleanup)
-- VACUUM FULL rebuilds indexes
DELETE FROM deletion_test WHERE content NOT LIKE '%test%';
-- We use \set VERBOSITY terse to avoid OID-specific error messages
\set VERBOSITY terse
VACUUM FULL deletion_test;
\set VERBOSITY default

-- Check statistics after VACUUM FULL attempt
SELECT split_part(
    split_part(bm25_debug_dump_index('deletion_idx')::text, 'total_docs: ', 2),
    E'\n', 1
) AS total_docs_after_vacuum_full;

-- Verify search still works
SELECT id, substring(content, 1, 30) as content_preview
FROM deletion_test
WHERE content <@> to_bm25query('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'deletion_idx');

-- Clean up
DROP TABLE deletion_test;
DROP EXTENSION pg_textsearch;
