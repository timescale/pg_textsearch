-- Cranfield Collection BM25 Benchmark - Insert Loading
-- Creates index on EMPTY table, then loads data via inserts
-- This measures insert performance into an already-indexed table

\set ON_ERROR_STOP on
\timing on

\echo 'Cranfield BM25 Benchmark - Insert Load Phase'
\echo '============================================='

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

-- Create BM25 index on the EMPTY table before loading data
\echo 'Creating BM25 index on empty table...'
CREATE INDEX cranfield_full_tapir_idx
    ON cranfield_full_documents USING bm25(full_text)
    WITH (text_config='english', k1=1.2, b=0.75);

-- Load the complete Cranfield dataset (inserts into indexed table)
\echo 'INSERT_TIME:'
\echo 'Loading Cranfield collection into indexed table...'
\i dataset.sql

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

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('cranfield_full_tapir_idx')
    ) as index_size,
    pg_relation_size(
        'cranfield_full_tapir_idx'
    ) as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size('cranfield_full_documents')
    ) as table_size,
    pg_total_relation_size(
        'cranfield_full_documents'
    ) as table_bytes;

-- Show segment statistics
\echo ''
\echo '=== Segment Statistics ==='
SELECT bm25_summarize_index('cranfield_full_tapir_idx');

\echo 'Insert load phase completed.'
