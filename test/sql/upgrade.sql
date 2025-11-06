-- Test extension upgrades from older versions

-- First ensure we can create the extension
CREATE EXTENSION IF NOT EXISTS pg_textsearch VERSION '0.0.3';

-- Test data to verify upgrades preserve functionality
CREATE TABLE upgrade_test (
    id SERIAL PRIMARY KEY,
    content TEXT NOT NULL
);

INSERT INTO upgrade_test (content) VALUES
    ('The quick brown fox jumps over the lazy dog'),
    ('PostgreSQL is a powerful open source database'),
    ('Full text search with BM25 ranking algorithm');

-- Create an index before upgrade
CREATE INDEX idx_upgrade_test_bm25 ON upgrade_test
USING bm25 (content) WITH (text_config = 'english');

-- Perform a search to verify index works
SELECT id, content <@> to_bm25query('search', 'idx_upgrade_test_bm25'::text) AS score
FROM upgrade_test
WHERE content @@ to_tsquery('english', 'search')
ORDER BY score DESC;

-- Test that ALTER EXTENSION UPDATE works (even if we're already on latest)
ALTER EXTENSION pg_textsearch UPDATE;

-- Verify the index still works after "upgrade"
SELECT id, content <@> to_bm25query('database', 'idx_upgrade_test_bm25'::text) AS score
FROM upgrade_test
WHERE content @@ to_tsquery('english', 'database')
ORDER BY score DESC;

-- Check extension version
SELECT extversion FROM pg_extension WHERE extname = 'pg_textsearch';

-- Clean up
DROP TABLE upgrade_test;
DROP EXTENSION pg_textsearch;
