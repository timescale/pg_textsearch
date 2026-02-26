-- Cranfield GIN+tsvector Baseline - Concurrent Insert Setup
-- Prepares staging table for pgbench concurrent inserts

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield GIN Baseline - Concurrent Insert Setup'
\echo '================================================='

-- Clean up any existing tables
\echo 'Cleaning up existing tables...'
DROP TABLE IF EXISTS cranfield_gin_documents CASCADE;
DROP TABLE IF EXISTS cranfield_gin_staging CASCADE;
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

-- Create staging table (raw data, no tsv column)
\echo 'Creating staging table...'
CREATE TABLE cranfield_gin_staging (
    staging_id SERIAL PRIMARY KEY,
    doc_id INTEGER,
    title TEXT,
    author TEXT,
    bibliography TEXT,
    content TEXT
);

INSERT INTO cranfield_gin_staging
    (doc_id, title, author, bibliography, content)
SELECT doc_id, title, author, bibliography, content
FROM cranfield_full_documents
ORDER BY doc_id;

-- Drop temp tables
DROP TABLE cranfield_full_documents CASCADE;
DROP TABLE cranfield_full_queries CASCADE;
DROP TABLE cranfield_full_expected_rankings CASCADE;

-- Create target GIN table with index (empty)
\echo 'Creating target GIN table with index...'
CREATE TABLE cranfield_gin_documents (
    doc_id INTEGER PRIMARY KEY,
    title TEXT,
    author TEXT,
    bibliography TEXT,
    content TEXT,
    full_text TEXT GENERATED ALWAYS AS (
        COALESCE(title, '') || ' ' ||
        COALESCE(author, '') || ' ' ||
        COALESCE(content, '')
    ) STORED,
    tsv tsvector GENERATED ALWAYS AS (
        to_tsvector('english',
            COALESCE(title, '') || ' ' ||
            COALESCE(author, '') || ' ' ||
            COALESCE(content, ''))
    ) STORED
);

CREATE INDEX cranfield_gin_idx
    ON cranfield_gin_documents USING gin(tsv);

-- Create sequence for pgbench inserts
CREATE SEQUENCE insert_seq;

-- Report staging row count
SELECT 'STAGING_ROW_COUNT: ' || count(*)
FROM cranfield_gin_staging;

\echo 'Concurrent insert setup complete.'
\echo 'Run pgbench with gin/pgbench-insert.sql next.'
