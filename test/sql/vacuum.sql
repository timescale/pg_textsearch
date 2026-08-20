-- Test VACUUM behavior with BM25 indexes

-- Ensure extension is loaded
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- True when the reclaim horizon may be pinned, leaving deleted tuples
-- HEAPTUPLE_RECENTLY_DEAD; see memtable_reclaim.sql for the rationale.
CREATE FUNCTION horizon_pinned() RETURNS boolean
LANGUAGE sql STABLE AS $$
    SELECT EXISTS (
               SELECT 1
               FROM pg_stat_activity
               WHERE pid <> pg_backend_pid()
                 AND backend_xmin IS NOT NULL
                 AND (datname = current_database()
                      OR backend_type = 'walsender')
           )
        OR EXISTS (SELECT 1 FROM pg_replication_slots WHERE xmin IS NOT NULL)
        OR EXISTS (SELECT 1 FROM pg_prepared_xacts);
$$;

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

-- Force index scan (small table would otherwise prefer seq scan)
SET enable_seqscan = off;

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
ORDER BY content <@> to_bm25query('test', 'vacuum_idx')
LIMIT 5;

-- Test VACUUM FULL (more aggressive cleanup)
-- VACUUM FULL rebuilds indexes
CREATE TEMP TABLE vacuum_full_horizon AS SELECT horizon_pinned() AS pinned;
DELETE FROM vacuum_test WHERE content NOT LIKE '%test%';
UPDATE vacuum_full_horizon SET pinned = pinned OR horizon_pinned();
-- We use \set VERBOSITY terse to avoid OID-specific error messages
\set VERBOSITY terse
-- The rebuild NOTICE reports the document count, which varies with the
-- horizon (VACUUM FULL copies recently-dead tuples too).  Silence it.
SET client_min_messages = warning;
VACUUM FULL vacuum_test;
RESET client_min_messages;
\set VERBOSITY default

-- Check statistics after VACUUM FULL.
SELECT CASE
           WHEN (SELECT pinned FROM vacuum_full_horizon) THEN '1'
           ELSE split_part(
                    split_part(bm25_dump_index('vacuum_idx')::text,
                               'total_docs: ', 2),
                    E'\n', 1)
       END AS total_docs_after_vacuum_full;

-- Verify search still works
SELECT id, substring(content, 1, 30) as content_preview
FROM vacuum_test
ORDER BY content <@> to_bm25query('test', 'vacuum_idx');

-- Clean up
DROP TABLE vacuum_test;
DROP FUNCTION horizon_pinned();
DROP EXTENSION pg_textsearch;
