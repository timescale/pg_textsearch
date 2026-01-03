-- ParadeDB pg_search Benchmark Setup
-- Creates BM25 index on MS-MARCO passages table
--
-- Prerequisites:
--   - MS-MARCO data loaded via datasets/msmarco/load.sql (without pg_textsearch index)
--   - ParadeDB pg_search extension available
--
-- Usage:
--   psql -f setup.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== ParadeDB pg_search Benchmark Setup ==='
\echo ''

-- Check if data is loaded
DO $$
DECLARE
    cnt bigint;
BEGIN
    SELECT COUNT(*) INTO cnt FROM msmarco_passages;
    IF cnt = 0 THEN
        RAISE EXCEPTION 'No passages loaded. Run datasets/msmarco/load.sql first.';
    END IF;
    RAISE NOTICE 'Found % passages in msmarco_passages table', cnt;
END $$;

-- Drop existing ParadeDB index if present
DROP INDEX IF EXISTS msmarco_pdb_idx;

-- Install pg_search extension (ParadeDB)
\echo 'Installing pg_search extension...'
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Create BM25 index using ParadeDB
-- Note: ParadeDB requires a key_field parameter
\echo ''
\echo '=== Building ParadeDB BM25 Index ==='
\echo 'Creating BM25 index on ~8.8M passages (this will take a while)...'

CREATE INDEX msmarco_pdb_idx ON msmarco_passages
USING bm25 (passage_id, passage_text)
WITH (key_field = 'passage_id');

-- Report index size
\echo ''
\echo '=== ParadeDB Index Size ==='
SELECT
    'PARADEDB_INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_pdb_idx')) as index_size,
    pg_relation_size('msmarco_pdb_idx') as index_bytes;

\echo ''
\echo '=== ParadeDB Setup Complete ==='
