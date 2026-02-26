-- Cranfield Collection BM25 Benchmark - Concurrent Insert Setup
-- Prepares staging table for pgbench concurrent inserts
-- After this script: cranfield_full_* tables exist with BM25 index,
-- cranfield_staging holds rows to be inserted via pgbench

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield BM25 Benchmark - Concurrent Insert Setup'
\echo '==================================================='

-- Clean up any existing tables
\echo 'Cleaning up existing tables...'
DROP TABLE IF EXISTS cranfield_full_documents CASCADE;
DROP TABLE IF EXISTS cranfield_full_queries CASCADE;
DROP TABLE IF EXISTS cranfield_full_expected_rankings CASCADE;
DROP TABLE IF EXISTS cranfield_staging CASCADE;
DROP TABLE IF EXISTS cranfield_queries_backup CASCADE;
DROP TABLE IF EXISTS cranfield_rankings_backup CASCADE;

-- Create tables and load dataset
\echo 'Creating tables...'
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

\echo 'Loading dataset...'
\i dataset.sql

-- Create staging table with document data
\echo 'Creating staging table...'
CREATE TABLE cranfield_staging (
    staging_id SERIAL PRIMARY KEY,
    doc_id INTEGER,
    title TEXT,
    author TEXT,
    bibliography TEXT,
    content TEXT
);

INSERT INTO cranfield_staging
    (doc_id, title, author, bibliography, content)
SELECT doc_id, title, author, bibliography, content
FROM cranfield_full_documents
ORDER BY doc_id;

-- Save queries and rankings into backup tables
\echo 'Backing up queries and rankings...'
CREATE TABLE cranfield_queries_backup AS
    SELECT * FROM cranfield_full_queries;
CREATE TABLE cranfield_rankings_backup AS
    SELECT * FROM cranfield_full_expected_rankings;

-- Drop all cranfield_full_* tables
\echo 'Dropping original tables...'
DROP TABLE cranfield_full_documents CASCADE;
DROP TABLE cranfield_full_queries CASCADE;
DROP TABLE cranfield_full_expected_rankings CASCADE;

-- Recreate cranfield_full_documents with BM25 index
\echo 'Recreating documents table with BM25 index...'
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

CREATE INDEX cranfield_full_tapir_idx
    ON cranfield_full_documents USING bm25(full_text)
    WITH (text_config='english', k1=1.2, b=0.75);

-- Recreate and repopulate queries and rankings
\echo 'Restoring queries and rankings...'
CREATE TABLE cranfield_full_queries (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);
INSERT INTO cranfield_full_queries
    SELECT * FROM cranfield_queries_backup;

CREATE TABLE cranfield_full_expected_rankings (
    query_id INTEGER NOT NULL,
    doc_id INTEGER NOT NULL,
    rank INTEGER NOT NULL,
    bm25_score FLOAT NOT NULL,
    PRIMARY KEY (query_id, doc_id)
);
INSERT INTO cranfield_full_expected_rankings
    SELECT * FROM cranfield_rankings_backup;

-- Drop backup tables
\echo 'Dropping backup tables...'
DROP TABLE cranfield_queries_backup;
DROP TABLE cranfield_rankings_backup;

-- Create sequence for pgbench inserts
DROP SEQUENCE IF EXISTS insert_seq;
CREATE SEQUENCE insert_seq;

-- Report staging row count
SELECT 'STAGING_ROW_COUNT: ' || count(*)
FROM cranfield_staging;

\echo 'Concurrent insert setup complete.'
\echo 'Run pgbench with insert/pgbench-insert.sql next.'
