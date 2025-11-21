-- Test lock upgrade from shared to exclusive within a single transaction
-- This exercises the tp_acquire_index_lock upgrade path

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test table and index
CREATE TABLE lock_upgrade_test (
    id SERIAL PRIMARY KEY,
    content TEXT NOT NULL
);

INSERT INTO lock_upgrade_test (content) VALUES
    ('database systems and concurrent access'),
    ('full text search indexing'),
    ('query optimization techniques');

CREATE INDEX lock_upgrade_idx ON lock_upgrade_test USING bm25(content)
    WITH (text_config='english', k1=1.2, b=0.75);

-- Force index scan (small tables default to seq scan)
SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- Test: Single transaction that first reads (shared lock) then writes (exclusive lock)
-- This should trigger the lock upgrade path in tp_acquire_index_lock
BEGIN;

-- First operation: SELECT acquires shared lock via index scan
SELECT id, content
FROM lock_upgrade_test
ORDER BY content <@> to_bm25query('database', 'lock_upgrade_idx')
LIMIT 5;

-- Second operation: INSERT in same transaction should upgrade to exclusive lock
INSERT INTO lock_upgrade_test (content) VALUES
    ('new document about database concurrency');

-- Verify the insert worked
SELECT COUNT(*) FROM lock_upgrade_test;

COMMIT;

-- Verify final state
SELECT COUNT(*) FROM lock_upgrade_test;

-- Test that subsequent operations work normally
SELECT id, content
FROM lock_upgrade_test
WHERE content <@> to_bm25query('database concurrency', 'lock_upgrade_idx') < -0.001
ORDER BY content <@> to_bm25query('database concurrency', 'lock_upgrade_idx')
LIMIT 5;

-- Clean up
DROP TABLE lock_upgrade_test CASCADE;
DROP EXTENSION pg_textsearch;
