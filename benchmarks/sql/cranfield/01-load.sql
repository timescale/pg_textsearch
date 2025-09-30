-- Cranfield Collection BM25 Benchmark - Data Loading
-- This benchmark loads the complete Cranfield collection and creates BM25 index
-- Timing: Data loading and index creation are timed but results not validated
-- Contains 1400 aerodynamics abstracts with 225 queries

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield BM25 Benchmark - Loading Phase'
\echo '======================================'

-- Clean up any existing tables
\echo 'Cleaning up existing tables...'
DROP TABLE IF EXISTS cranfield_full_documents CASCADE;
DROP TABLE IF EXISTS cranfield_full_queries CASCADE;
DROP TABLE IF EXISTS cranfield_full_expected_rankings CASCADE;

-- Create tables for Cranfield dataset
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

-- Load the complete Cranfield dataset
\echo 'Loading complete Cranfield collection (1400 documents, 225 queries)...'
\i dataset.sql

-- Create BM25 index for Cranfield documents
\echo 'Building BM25 index (this may take time)...'
CREATE INDEX cranfield_full_tapir_idx ON cranfield_full_documents USING pg_textsearch(full_text)
    WITH (text_config='english', k1=1.2, b=0.75);

-- Verify data loading
\echo 'Data loading verification:'
SELECT
    'Documents loaded:' as metric,
    COUNT(*) as count
FROM cranfield_full_documents
UNION ALL
SELECT
    'Queries loaded:' as metric,
    COUNT(*) as count
FROM cranfield_full_queries
UNION ALL
SELECT
    'Expected rankings:' as metric,
    COUNT(*) as count
FROM cranfield_full_expected_rankings
ORDER BY metric;

\echo 'Load phase completed. Index ready for queries.'
