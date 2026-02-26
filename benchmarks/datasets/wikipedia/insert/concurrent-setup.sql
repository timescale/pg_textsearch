-- Wikipedia Dataset - Concurrent Insert Setup
-- Loads articles into a staging table, creates empty target table
-- with BM25 index, and prepares a sequence for pgbench-driven
-- concurrent inserts.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f insert/concurrent-setup.sql
--
-- After running this script, use pgbench with pgbench-insert.sql
-- to drive concurrent inserts from staging into the indexed table.

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia Concurrent Insert Setup ==='
\echo ''

-- Clean up existing tables
DROP TABLE IF EXISTS wikipedia_articles CASCADE;
DROP TABLE IF EXISTS wikipedia_staging CASCADE;
DROP SEQUENCE IF EXISTS insert_seq;

-- Create staging table
\echo 'Creating staging table...'
CREATE TABLE wikipedia_staging (
    staging_id SERIAL PRIMARY KEY,
    article_id INTEGER,
    title TEXT,
    content TEXT
);

-- Load articles into staging
\echo 'Loading articles into staging table...'
\copy wikipedia_staging(article_id, title, content) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); gsub(/\"/, \"\\\"\\\"\", \$3); print \$1, \"\\\"\" \$2 \"\\\"\", \"\\\"\" \$3 \"\\\"\"}" "$DATA_DIR/wikipedia.tsv"' WITH (FORMAT csv);

-- Create target articles table
\echo 'Creating target articles table...'
CREATE TABLE wikipedia_articles (
    article_id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL
);

-- Create BM25 index on empty target table
\echo 'Creating BM25 index on empty target table...'
CREATE INDEX wikipedia_bm25_idx ON wikipedia_articles
    USING bm25(content) WITH (text_config='english');

-- Create sequence for pgbench to draw row IDs from
\echo 'Creating insert sequence...'
CREATE SEQUENCE insert_seq;

-- Report staging row count
\echo ''
\echo '=== Setup Verification ==='
SELECT 'STAGING_ROW_COUNT:' as label,
    COUNT(*)::text as count
FROM wikipedia_staging;

\echo ''
\echo '=== Concurrent Insert Setup Complete ==='
\echo 'Run pgbench with pgbench-insert.sql to start inserts'
