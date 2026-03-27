-- Wikipedia Dataset - ParadeDB Concurrent Insert Setup
-- Loads articles into a staging table, creates empty target
-- table with ParadeDB BM25 index, and prepares a sequence for
-- pgbench-driven concurrent inserts.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f paradedb/concurrent-setup.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia ParadeDB Concurrent Insert Setup ==='
\echo ''

-- Ensure ParadeDB extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up existing tables
DROP TABLE IF EXISTS wikipedia_articles_paradedb CASCADE;
DROP TABLE IF EXISTS wikipedia_paradedb_staging CASCADE;
DROP SEQUENCE IF EXISTS insert_seq;

-- Create staging table
\echo 'Creating staging table...'
CREATE TABLE wikipedia_paradedb_staging (
    staging_id SERIAL PRIMARY KEY,
    article_id INTEGER,
    title TEXT,
    content TEXT
);

-- Load articles into staging
\echo 'Loading articles into staging table...'
\copy wikipedia_paradedb_staging(article_id, title, content) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); gsub(/\"/, \"\\\"\\\"\", \$3); print \$1, \"\\\"\" \$2 \"\\\"\", \"\\\"\" \$3 \"\\\"\"}" "$DATA_DIR/wikipedia.tsv"' WITH (FORMAT csv);

-- Create target articles table
\echo 'Creating target articles table...'
CREATE TABLE wikipedia_articles_paradedb (
    article_id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL
);

-- Create ParadeDB BM25 index on empty target table
\echo 'Creating ParadeDB BM25 index on empty table...'
CREATE INDEX wikipedia_paradedb_idx
    ON wikipedia_articles_paradedb
    USING bm25 (article_id, content)
    WITH (
        key_field = 'article_id',
        text_fields = '{
            "content": {
                "tokenizer": {
                    "type": "default",
                    "stopwords_language": "English",
                    "stemmer": "English"
                }
            }
        }'
    );

-- Create sequence for pgbench inserts
\echo 'Creating insert sequence...'
CREATE SEQUENCE insert_seq;

-- Report staging row count
\echo ''
\echo '=== Setup Verification ==='
SELECT 'STAGING_ROW_COUNT:' as label,
    COUNT(*)::text as count
FROM wikipedia_paradedb_staging;

\echo ''
\echo '=== ParadeDB Concurrent Insert Setup Complete ==='
\echo 'Run pgbench with paradedb/pgbench-insert.sql'
