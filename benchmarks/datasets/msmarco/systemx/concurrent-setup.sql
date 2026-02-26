-- MS MARCO Passage Ranking - System X Concurrent Insert Setup
-- Loads passages into a staging table, creates empty target
-- table with System X BM25 index, and prepares a sequence for
-- pgbench-driven concurrent inserts.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f systemx/concurrent-setup.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO System X Concurrent Insert Setup ==='
\echo ''

-- Ensure System X extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_passages_systemx CASCADE;
DROP TABLE IF EXISTS msmarco_queries_systemx CASCADE;
DROP TABLE IF EXISTS msmarco_qrels_systemx CASCADE;
DROP TABLE IF EXISTS msmarco_systemx_staging CASCADE;
DROP SEQUENCE IF EXISTS insert_seq;

-- Create staging table
\echo 'Creating staging table...'
CREATE TABLE msmarco_systemx_staging (
    staging_id SERIAL PRIMARY KEY,
    passage_id INTEGER,
    passage_text TEXT
);

-- Load passages into staging
\echo 'Loading passages into staging table...'
\copy msmarco_systemx_staging(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \$1, \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Create target passages table
\echo 'Creating target passages table...'
CREATE TABLE msmarco_passages_systemx (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL
);

-- Create System X BM25 index on empty target table
\echo 'Creating System X BM25 index on empty table...'
CREATE INDEX msmarco_systemx_idx ON msmarco_passages_systemx
    USING bm25 (passage_id, passage_text)
    WITH (
        key_field = 'passage_id',
        text_fields = '{
            "passage_text": {
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
FROM msmarco_systemx_staging;

\echo ''
\echo '=== System X Concurrent Insert Setup Complete ==='
\echo 'Run pgbench with systemx/pgbench-insert.sql'
