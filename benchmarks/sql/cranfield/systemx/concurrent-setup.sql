-- Cranfield Collection - System X Concurrent Insert Setup
-- Prepares staging table for pgbench concurrent inserts
-- After this script: cranfield_systemx_documents exists with
-- System X BM25 index, cranfield_systemx_staging holds rows
-- to be inserted via pgbench

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield System X - Concurrent Insert Setup'
\echo '============================================='

-- Ensure System X extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up any existing tables
\echo 'Cleaning up existing tables...'
DROP TABLE IF EXISTS cranfield_systemx_documents CASCADE;
DROP TABLE IF EXISTS cranfield_systemx_queries CASCADE;
DROP TABLE IF EXISTS cranfield_systemx_expected_rankings CASCADE;
DROP TABLE IF EXISTS cranfield_systemx_staging CASCADE;
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
CREATE TABLE cranfield_systemx_staging (
    staging_id SERIAL PRIMARY KEY,
    doc_id INTEGER,
    title TEXT,
    author TEXT,
    bibliography TEXT,
    content TEXT
);

INSERT INTO cranfield_systemx_staging
    (doc_id, title, author, bibliography, content)
SELECT doc_id, title, author, bibliography, content
FROM cranfield_full_documents
ORDER BY doc_id;

-- Drop temp tables
DROP TABLE cranfield_full_documents CASCADE;
DROP TABLE cranfield_full_queries CASCADE;
DROP TABLE cranfield_full_expected_rankings CASCADE;

-- Create target System X table with BM25 index (empty)
\echo 'Creating target System X table with BM25 index...'
CREATE TABLE cranfield_systemx_documents (
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

CREATE INDEX cranfield_systemx_idx
    ON cranfield_systemx_documents
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
FROM cranfield_systemx_staging;

\echo 'Concurrent insert setup complete.'
\echo 'Run pgbench with systemx/pgbench-insert.sql next.'
