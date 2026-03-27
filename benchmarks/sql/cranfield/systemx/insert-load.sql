-- Cranfield Collection - System X Insert Loading
-- Creates System X BM25 index on EMPTY table, then loads data
-- via inserts to measure insert-path indexing performance

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield System X - Insert Load Phase'
\echo '======================================='

-- Ensure System X extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up any existing tables
\echo 'Cleaning up existing tables...'
DROP TABLE IF EXISTS cranfield_systemx_documents CASCADE;
DROP TABLE IF EXISTS cranfield_systemx_queries CASCADE;
DROP TABLE IF EXISTS cranfield_systemx_expected_rankings CASCADE;
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

-- Create the System X target tables
\echo 'Creating System X documents table...'
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

CREATE TABLE cranfield_systemx_queries (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);

CREATE TABLE cranfield_systemx_expected_rankings (
    query_id INTEGER NOT NULL,
    doc_id INTEGER NOT NULL,
    rank INTEGER NOT NULL,
    bm25_score FLOAT NOT NULL,
    PRIMARY KEY (query_id, doc_id)
);

-- Create System X BM25 index on the EMPTY table
\echo 'Creating System X BM25 index on empty table...'
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

-- Load dataset into temp tables
\echo 'Loading Cranfield collection...'
\i dataset.sql

-- Insert data into indexed System X table
\echo 'INSERT_TIME:'
\echo 'Inserting data into indexed System X table...'
INSERT INTO cranfield_systemx_documents
    (doc_id, title, author, bibliography, content)
SELECT doc_id, title, author, bibliography, content
FROM cranfield_full_documents;

INSERT INTO cranfield_systemx_queries
    SELECT * FROM cranfield_full_queries;
INSERT INTO cranfield_systemx_expected_rankings
    SELECT * FROM cranfield_full_expected_rankings;

-- Drop temp tables
DROP TABLE cranfield_full_documents CASCADE;
DROP TABLE cranfield_full_queries CASCADE;
DROP TABLE cranfield_full_expected_rankings CASCADE;

-- Compact index segments (System X merges during VACUUM)
\echo 'INDEX_VACUUM:'
VACUUM cranfield_systemx_documents;

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('cranfield_systemx_idx')
    ) as index_size,
    pg_relation_size(
        'cranfield_systemx_idx'
    ) as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size(
            'cranfield_systemx_documents')
    ) as table_size,
    pg_total_relation_size(
        'cranfield_systemx_documents') as table_bytes;

\echo 'System X insert load phase completed.'
