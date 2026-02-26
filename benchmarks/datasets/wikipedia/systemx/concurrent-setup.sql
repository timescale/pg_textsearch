-- Wikipedia Dataset - System X Concurrent Insert Setup
-- Loads articles into a staging table, creates empty target
-- table with System X BM25 index, and prepares a sequence for
-- pgbench-driven concurrent inserts.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f systemx/concurrent-setup.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia System X Concurrent Insert Setup ==='
\echo ''

-- Ensure System X extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up existing tables
DROP TABLE IF EXISTS wikipedia_articles_systemx CASCADE;
DROP TABLE IF EXISTS wikipedia_systemx_staging CASCADE;
DROP SEQUENCE IF EXISTS insert_seq;

-- Create staging table
\echo 'Creating staging table...'
CREATE TABLE wikipedia_systemx_staging (
    staging_id SERIAL PRIMARY KEY,
    article_id INTEGER,
    title TEXT,
    content TEXT
);

-- Load articles into staging
\echo 'Loading articles into staging table...'
\copy wikipedia_systemx_staging(article_id, title, content) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); gsub(/\"/, \"\\\"\\\"\", \$3); print \$1, \"\\\"\" \$2 \"\\\"\", \"\\\"\" \$3 \"\\\"\"}" "$DATA_DIR/wikipedia.tsv"' WITH (FORMAT csv);

-- Create target articles table
\echo 'Creating target articles table...'
CREATE TABLE wikipedia_articles_systemx (
    article_id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL
);

-- Create System X BM25 index on empty target table
\echo 'Creating System X BM25 index on empty table...'
CREATE INDEX wikipedia_systemx_idx
    ON wikipedia_articles_systemx
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
FROM wikipedia_systemx_staging;

\echo ''
\echo '=== System X Concurrent Insert Setup Complete ==='
\echo 'Run pgbench with systemx/pgbench-insert.sql'
