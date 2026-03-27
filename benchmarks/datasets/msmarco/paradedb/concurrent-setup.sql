-- MS MARCO Passage Ranking - ParadeDB Concurrent Insert Setup
-- Loads passages into a staging table, creates empty target
-- table with ParadeDB BM25 index, and prepares a sequence for
-- pgbench-driven concurrent inserts.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f paradedb/concurrent-setup.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO ParadeDB Concurrent Insert Setup ==='
\echo ''

-- Ensure ParadeDB extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_passages_paradedb CASCADE;
DROP TABLE IF EXISTS msmarco_queries_paradedb CASCADE;
DROP TABLE IF EXISTS msmarco_qrels_paradedb CASCADE;
DROP TABLE IF EXISTS msmarco_paradedb_staging CASCADE;
DROP SEQUENCE IF EXISTS insert_seq;

-- Create staging table
\echo 'Creating staging table...'
CREATE TABLE msmarco_paradedb_staging (
    staging_id SERIAL PRIMARY KEY,
    passage_id INTEGER,
    passage_text TEXT
);

-- Load passages into staging
\echo 'Loading passages into staging table...'
\copy msmarco_paradedb_staging(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \$1, \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Create target passages table
\echo 'Creating target passages table...'
CREATE TABLE msmarco_passages_paradedb (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL
);

-- Create ParadeDB BM25 index on empty target table
\echo 'Creating ParadeDB BM25 index on empty table...'
CREATE INDEX msmarco_paradedb_idx ON msmarco_passages_paradedb
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
FROM msmarco_paradedb_staging;

\echo ''
\echo '=== ParadeDB Concurrent Insert Setup Complete ==='
\echo 'Run pgbench with paradedb/pgbench-insert.sql'
