-- Wikipedia GIN+tsvector Baseline - Concurrent Insert Setup
-- Loads articles into a staging table, creates empty target table
-- with GIN index, and prepares a sequence for pgbench-driven
-- concurrent inserts.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f gin/concurrent-setup.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia GIN Concurrent Insert Setup ==='
\echo ''

-- Clean up existing tables
DROP TABLE IF EXISTS wikipedia_gin_articles CASCADE;
DROP TABLE IF EXISTS wikipedia_gin_staging CASCADE;
DROP SEQUENCE IF EXISTS insert_seq;

-- Create staging table (raw data, no tsv column)
\echo 'Creating staging table...'
CREATE TABLE wikipedia_gin_staging (
    staging_id SERIAL PRIMARY KEY,
    article_id INTEGER,
    title TEXT,
    content TEXT
);

-- Load articles into staging
\echo 'Loading articles into staging table...'
\copy wikipedia_gin_staging(article_id, title, content) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); gsub(/\"/, \"\\\"\\\"\", \$3); print \$1, \"\\\"\" \$2 \"\\\"\", \"\\\"\" \$3 \"\\\"\"}" "$DATA_DIR/wikipedia.tsv"' WITH (FORMAT csv);

-- Create target articles table with GIN index
\echo 'Creating target articles table...'
CREATE TABLE wikipedia_gin_articles (
    article_id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL,
    tsv tsvector GENERATED ALWAYS AS (
        to_tsvector('english', content)
    ) STORED
);

\echo 'Creating GIN index on empty target table...'
CREATE INDEX wikipedia_gin_idx
    ON wikipedia_gin_articles USING gin(tsv);

-- Create sequence for pgbench inserts
\echo 'Creating insert sequence...'
CREATE SEQUENCE insert_seq;

-- Report staging row count
\echo ''
\echo '=== Setup Verification ==='
SELECT 'STAGING_ROW_COUNT:' as label,
    COUNT(*)::text as count
FROM wikipedia_gin_staging;

\echo ''
\echo '=== GIN Concurrent Insert Setup Complete ==='
\echo 'Run pgbench with gin/pgbench-insert.sql'
