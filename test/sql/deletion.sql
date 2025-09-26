-- Test handling of row deletions in indexed tables

-- Ensure extension is loaded
CREATE EXTENSION IF NOT EXISTS tapir;

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
USING tapir (content)
WITH (text_config = 'english');

-- Initial search - should find documents with "test"
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_tpquery('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_tpquery('test', 'deletion_idx') DESC
LIMIT 5;

-- Delete one of the documents that contains "test"
DELETE FROM deletion_test WHERE id = 3;

-- Search again - deleted document should not appear
-- (Note: corpus statistics won't be updated, but shouldn't crash)
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_tpquery('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_tpquery('test', 'deletion_idx') DESC
LIMIT 5;

-- Delete a document that doesn't contain "test"
DELETE FROM deletion_test WHERE id = 4;

-- Search again - should still work
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_tpquery('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_tpquery('test', 'deletion_idx') DESC
LIMIT 5;

-- Delete all remaining documents with "test"
DELETE FROM deletion_test WHERE content LIKE '%test%';

-- Search again - should return no results (but not crash)
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_tpquery('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_tpquery('test', 'deletion_idx') DESC
LIMIT 5;

-- Insert new documents after deletions
INSERT INTO deletion_test (content) VALUES
    ('new document with test keyword'),
    ('another new document');

-- Search again - should find the new document
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_tpquery('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_tpquery('test', 'deletion_idx') DESC
LIMIT 5;

-- Test with empty table after deleting everything
DELETE FROM deletion_test;

-- Search in empty table - should return no results
SELECT id, substring(content, 1, 40) as content_preview
FROM deletion_test
WHERE content <@> to_tpquery('test', 'deletion_idx') < -0.001
ORDER BY content <@> to_tpquery('test', 'deletion_idx') DESC
LIMIT 5;

-- Clean up
DROP TABLE deletion_test;
