-- Cranfield Collection BM25 Benchmark - Data Loading (System X)
-- This benchmark loads the complete Cranfield collection and creates System X BM25 index
-- Contains 1400 aerodynamics abstracts with 225 queries

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield BM25 Benchmark - Loading Phase (System X)'
\echo '======================================'

-- Ensure System X extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up any existing tables
\echo 'Cleaning up existing tables...'
DROP TABLE IF EXISTS cranfield_systemx_documents CASCADE;
DROP TABLE IF EXISTS cranfield_systemx_queries CASCADE;
DROP TABLE IF EXISTS cranfield_systemx_expected_rankings CASCADE;

-- Create tables for Cranfield dataset
\echo 'Creating tables...'
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

-- Load the complete Cranfield dataset using the shared dataset
-- We need to temporarily create the original tables and copy data
\echo 'Loading complete Cranfield collection (1400 documents, 225 queries)...'

-- Create temp tables with original names for dataset.sql compatibility
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
\i dataset.sql

-- Copy to systemx tables
INSERT INTO cranfield_systemx_documents (doc_id, title, author, bibliography, content)
SELECT doc_id, title, author, bibliography, content FROM cranfield_full_documents;

INSERT INTO cranfield_systemx_queries SELECT * FROM cranfield_full_queries;
INSERT INTO cranfield_systemx_expected_rankings SELECT * FROM cranfield_full_expected_rankings;

-- Drop temp tables
DROP TABLE cranfield_full_documents CASCADE;
DROP TABLE cranfield_full_queries CASCADE;
DROP TABLE cranfield_full_expected_rankings CASCADE;

-- Create System X BM25 index
\echo 'Building System X BM25 index...'
CREATE INDEX cranfield_systemx_idx ON cranfield_systemx_documents
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

-- Verify data loading
\echo 'Data loading verification:'
SELECT
    'Documents loaded:' as metric,
    COUNT(*) as count
FROM cranfield_systemx_documents
UNION ALL
SELECT
    'Queries loaded:' as metric,
    COUNT(*) as count
FROM cranfield_systemx_queries
UNION ALL
SELECT
    'Expected rankings:' as metric,
    COUNT(*) as count
FROM cranfield_systemx_expected_rankings
ORDER BY metric;

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('cranfield_systemx_idx')) as index_size,
    pg_relation_size('cranfield_systemx_idx') as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('cranfield_systemx_documents')) as table_size,
    pg_total_relation_size('cranfield_systemx_documents') as table_bytes;

\echo 'Load phase completed. Index ready for queries.'
