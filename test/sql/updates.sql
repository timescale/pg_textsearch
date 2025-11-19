-- Test UPDATE operations with BM25 indexes
-- This test reproduces the NULL index_state crash reported in production

-- Install the extension if not already installed
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Test basic UPDATE operations
CREATE TABLE update_test (
    id SERIAL PRIMARY KEY,
    content TEXT,
    meta JSONB
);

-- Create BM25 index
CREATE INDEX idx_update_test_content ON update_test
    USING bm25 (content)
    WITH (text_config = 'english');

-- Insert initial data
INSERT INTO update_test (content, meta) VALUES
    ('The quick brown fox jumps over the lazy dog', '{"type": "test", "version": 1}'::jsonb),
    ('PostgreSQL is a powerful database system', '{"type": "database", "version": 1}'::jsonb),
    ('Full text search with BM25 ranking is efficient', '{"type": "search", "version": 1}'::jsonb);

-- Test simple UPDATE
UPDATE update_test
SET content = 'The quick brown fox runs fast'
WHERE id = 1;

-- Test UPDATE with RETURNING
UPDATE update_test
SET content = 'PostgreSQL is an advanced relational database',
    meta = meta || '{"last_updated": "2025-01-01", "version": 2}'::jsonb
WHERE id = 2
RETURNING id, content;

-- Test UPDATE matching multiple rows
UPDATE update_test
SET meta = meta || '{"updated": true}'::jsonb
WHERE content <@> to_bm25query('database', 'idx_update_test_content') > -5;

-- Test UPDATE with complex WHERE clause (similar to production crash)
UPDATE update_test
SET
    content = 'Advanced full text search with BM25 scoring',
    meta = meta || '{"last_updated": "2025-01-15", "num_updates": 3}'::jsonb
WHERE meta @> '{"type": "search"}'::jsonb
RETURNING id;

-- Test UPDATE with NULL content
UPDATE update_test
SET content = NULL
WHERE id = 1;

-- Test UPDATE from NULL to non-NULL
UPDATE update_test
SET content = 'Previously NULL content now has text'
WHERE id = 1;

-- Test bulk UPDATE
UPDATE update_test
SET content = content || ' - updated in bulk'
WHERE id IN (2, 3);

-- Test UPDATE with subquery
UPDATE update_test
SET content = (SELECT 'Content from subquery for id ' || u.id::text FROM update_test u WHERE u.id = update_test.id)
WHERE id = 2;

-- Verify data after updates
SELECT id, substring(content, 1, 50) as content_preview, meta->>'type' as type
FROM update_test
ORDER BY id;

-- Test UPDATE in transaction with ROLLBACK
BEGIN;
UPDATE update_test SET content = 'Transaction test' WHERE id = 1;
SELECT id, substring(content, 1, 30) as preview FROM update_test WHERE id = 1;
ROLLBACK;

-- Verify content was not changed
SELECT id, substring(content, 1, 30) as preview FROM update_test WHERE id = 1;

-- Test UPDATE in transaction with COMMIT
BEGIN;
UPDATE update_test SET content = 'Committed transaction update' WHERE id = 1;
COMMIT;

-- Verify content was changed
SELECT id, substring(content, 1, 30) as preview FROM update_test WHERE id = 1;

-- Test concurrent UPDATE scenario (serialized for single-session test)
-- In production, this would be multiple concurrent sessions
UPDATE update_test SET content = 'Concurrent update 1' WHERE id = 2;
UPDATE update_test SET content = 'Concurrent update 2' WHERE id = 2;

-- Test UPDATE with inheritance (should fail gracefully)
CREATE TABLE update_parent (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE TABLE update_child () INHERITS (update_parent);

-- This should fail with a clear error about inheritance not being supported
-- We use DO block to catch the error and show it
DO $$
BEGIN
    CREATE INDEX idx_update_parent_content ON update_parent
        USING bm25 (content)
        WITH (text_config = 'english');
EXCEPTION
    WHEN OTHERS THEN
        RAISE NOTICE 'Expected error: %', SQLERRM;
END;
$$;

-- Clean up
DROP TABLE IF EXISTS update_child;
DROP TABLE IF EXISTS update_parent;
DROP TABLE IF EXISTS update_test CASCADE;
DROP EXTENSION IF EXISTS pg_textsearch CASCADE;
