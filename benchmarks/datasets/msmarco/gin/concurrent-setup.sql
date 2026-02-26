-- MS MARCO GIN+tsvector Baseline - Concurrent Insert Setup
-- Loads passages into a staging table, creates empty target table
-- with GIN index, and prepares a sequence for pgbench-driven
-- concurrent inserts.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f gin/concurrent-setup.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO GIN Concurrent Insert Setup ==='
\echo ''

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_gin_passages CASCADE;
DROP TABLE IF EXISTS msmarco_gin_staging CASCADE;
DROP SEQUENCE IF EXISTS insert_seq;

-- Create staging table (raw data, no tsv column)
\echo 'Creating staging table...'
CREATE TABLE msmarco_gin_staging (
    staging_id SERIAL PRIMARY KEY,
    passage_id INTEGER,
    passage_text TEXT
);

-- Load passages into staging
\echo 'Loading passages into staging table...'
\copy msmarco_gin_staging(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \$1, \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Create target passages table with GIN index
\echo 'Creating target passages table...'
CREATE TABLE msmarco_gin_passages (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL,
    tsv tsvector GENERATED ALWAYS AS (
        to_tsvector('english', passage_text)
    ) STORED
);

\echo 'Creating GIN index on empty target table...'
CREATE INDEX msmarco_gin_idx
    ON msmarco_gin_passages USING gin(tsv);

-- Create sequence for pgbench inserts
\echo 'Creating insert sequence...'
CREATE SEQUENCE insert_seq;

-- Report staging row count
\echo ''
\echo '=== Setup Verification ==='
SELECT 'STAGING_ROW_COUNT:' as label,
    COUNT(*)::text as count
FROM msmarco_gin_staging;

\echo ''
\echo '=== GIN Concurrent Insert Setup Complete ==='
\echo 'Run pgbench with gin/pgbench-insert.sql'
