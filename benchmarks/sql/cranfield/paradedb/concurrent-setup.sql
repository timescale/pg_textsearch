-- Cranfield Collection - ParadeDB Concurrent Insert Setup
-- Prepares staging table for pgbench concurrent inserts
-- After this script: cranfield_paradedb_documents exists with
-- ParadeDB BM25 index, cranfield_paradedb_staging holds rows
-- to be inserted via pgbench

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield ParadeDB - Concurrent Insert Setup'
\echo '============================================='

-- Ensure ParadeDB extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up any existing tables
\echo 'Cleaning up existing tables...'
DROP TABLE IF EXISTS cranfield_paradedb_documents CASCADE;
DROP TABLE IF EXISTS cranfield_paradedb_queries CASCADE;
DROP TABLE IF EXISTS cranfield_paradedb_expected_rankings CASCADE;
DROP TABLE IF EXISTS cranfield_paradedb_staging CASCADE;
DROP TABLE IF EXISTS cranfield_full_documents CASCADE;
DROP TABLE IF EXISTS cranfield_full_queries CASCADE;
DROP TABLE IF EXISTS cranfield_full_expected_rankings CASCADE;
DROP SEQUENCE IF EXISTS insert_seq;

-- Create temp tables for dataset.sql loading
\echo 'Creating tables for dataset loading...'
CREATE TABLE cranfield_full_documents (
    doc_id INTEGER PRIMARY KEY,
    title TEXT,
    author TEXT,
    bibliography TEXT,
    content TEXT,
    full_text TEXT GENERATED ALWAYS AS (
        COALESCE(title, '') || ' ' ||
        COALESCE(author, '') || ' ' ||
        COALESCE(content, '')
    ) STORED
);

CREATE TABLE cranfield_full_queries (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);

CREATE TABLE cranfield_full_expected_rankings (
    query_id INTEGER NOT NULL,
    doc_id INTEGER NOT NULL,
    rank INTEGER NOT NULL,
    bm25_score FLOAT NOT NULL,
    PRIMARY KEY (query_id, doc_id)
);

-- Load dataset
\echo 'Loading Cranfield collection...'
\i dataset.sql

-- Create staging table with document data
\echo 'Creating staging table...'
CREATE TABLE cranfield_paradedb_staging (
    staging_id SERIAL PRIMARY KEY,
    doc_id INTEGER,
    title TEXT,
    author TEXT,
    bibliography TEXT,
    content TEXT
);

INSERT INTO cranfield_paradedb_staging
    (doc_id, title, author, bibliography, content)
SELECT doc_id, title, author, bibliography, content
FROM cranfield_full_documents
ORDER BY doc_id;

-- Drop temp tables
DROP TABLE cranfield_full_documents CASCADE;
DROP TABLE cranfield_full_queries CASCADE;
DROP TABLE cranfield_full_expected_rankings CASCADE;

-- Create target ParadeDB table with BM25 index (empty)
\echo 'Creating target ParadeDB table with BM25 index...'
CREATE TABLE cranfield_paradedb_documents (
    doc_id INTEGER PRIMARY KEY,
    title TEXT,
    author TEXT,
    bibliography TEXT,
    content TEXT,
    full_text TEXT GENERATED ALWAYS AS (
        COALESCE(title, '') || ' ' ||
        COALESCE(author, '') || ' ' ||
        COALESCE(content, '')
    ) STORED
);

CREATE INDEX cranfield_paradedb_idx
    ON cranfield_paradedb_documents
    USING bm25 (doc_id, full_text)
    WITH (
        key_field = 'doc_id',
        text_fields = '{
            "full_text": {
                "tokenizer": {
                    "type": "default",
                    "stopwords_language": "English",
                    "stemmer": "English"
                }
            }
        }'
    );

-- Create sequence for pgbench inserts
CREATE SEQUENCE insert_seq;

-- Report staging row count
SELECT 'STAGING_ROW_COUNT: ' || count(*)
FROM cranfield_paradedb_staging;

\echo 'Concurrent insert setup complete.'
\echo 'Run pgbench with paradedb/pgbench-insert.sql next.'
