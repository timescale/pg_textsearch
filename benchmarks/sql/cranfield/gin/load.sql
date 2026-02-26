-- Cranfield GIN+tsvector Baseline - Data Loading
-- Loads data first, then creates GIN index on populated table
-- Measures CREATE INDEX time and final sizes

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield GIN Baseline - Loading Phase'
\echo '======================================='

-- Clean up any existing tables
\echo 'Cleaning up existing tables...'
DROP TABLE IF EXISTS cranfield_gin_documents CASCADE;
DROP TABLE IF EXISTS cranfield_full_documents CASCADE;
DROP TABLE IF EXISTS cranfield_full_queries CASCADE;
DROP TABLE IF EXISTS cranfield_full_expected_rankings CASCADE;

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

-- Create the GIN target table (without index)
\echo 'Creating GIN documents table...'
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

-- Copy data into GIN table
\echo 'Copying data into GIN table...'
INSERT INTO cranfield_gin_documents
    (doc_id, title, author, bibliography, content)
SELECT doc_id, title, author, bibliography, content
FROM cranfield_full_documents;

-- Drop temp tables
DROP TABLE cranfield_full_documents CASCADE;
DROP TABLE cranfield_full_queries CASCADE;
DROP TABLE cranfield_full_expected_rankings CASCADE;

-- Create GIN index on populated table (timed)
\echo 'Building GIN index...'
CREATE INDEX cranfield_gin_idx
    ON cranfield_gin_documents USING gin(tsv);

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('cranfield_gin_idx')
    ) as index_size,
    pg_relation_size('cranfield_gin_idx')
        as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size(
            'cranfield_gin_documents')
    ) as table_size,
    pg_total_relation_size(
        'cranfield_gin_documents') as table_bytes;

\echo 'GIN load phase completed.'
