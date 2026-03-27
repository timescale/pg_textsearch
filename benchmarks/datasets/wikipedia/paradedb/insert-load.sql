-- Wikipedia Dataset - ParadeDB Insert Loading
-- Creates ParadeDB BM25 index on EMPTY table, then loads data
-- via COPY to measure insert-path indexing performance.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f paradedb/insert-load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia ParadeDB Insert Benchmark - Data Loading ==='
\echo 'Index created BEFORE loading articles'
\echo ''

-- Ensure ParadeDB extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up existing tables
DROP TABLE IF EXISTS wikipedia_articles_paradedb CASCADE;

-- Create articles table
\echo 'Creating articles table...'
CREATE TABLE wikipedia_articles_paradedb (
    article_id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL
);

-- Create ParadeDB BM25 index on the empty table
\echo 'Creating ParadeDB BM25 index on empty table...'
CREATE INDEX wikipedia_paradedb_idx
    ON wikipedia_articles_paradedb
    USING bm25 (article_id, content)
    WITH (
        key_field = 'article_id',
        text_fields = '{
            "content": {
                "tokenizer": {
                    "type": "default",
                    "stopwords_language": "English",
                    "stemmer": "English"
                }
            }
        }'
    );

-- Load articles into the pre-indexed table
\echo ''
\echo 'INSERT_TIME:'
\echo 'Loading articles (this may take a while)...'
\copy wikipedia_articles_paradedb(article_id, title, content) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); gsub(/\"/, \"\\\"\\\"\", \$3); print \$1, \"\\\"\" \$2 \"\\\"\", \"\\\"\" \$3 \"\\\"\"}" "$DATA_DIR/wikipedia.tsv"' WITH (FORMAT csv);

-- Compact index segments (ParadeDB merges during VACUUM)
\echo 'INDEX_VACUUM:'
VACUUM wikipedia_articles_paradedb;

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT
    'Articles loaded' as metric,
    COUNT(*)::text as value
FROM wikipedia_articles_paradedb
UNION ALL
SELECT
    'Avg content length (chars)',
    ROUND(AVG(LENGTH(content)))::text
FROM wikipedia_articles_paradedb
UNION ALL
SELECT
    'Total text size (MB)',
    ROUND(
        SUM(LENGTH(content)) / 1024.0 / 1024.0,
        1
    )::text
FROM wikipedia_articles_paradedb;

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('wikipedia_paradedb_idx')
    ) as index_size,
    pg_relation_size('wikipedia_paradedb_idx')
        as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size(
            'wikipedia_articles_paradedb')
    ) as table_size,
    pg_total_relation_size(
        'wikipedia_articles_paradedb') as table_bytes;

\echo ''
\echo '=== Wikipedia ParadeDB Insert Benchmark Complete ==='
\echo 'Ready for query benchmarks'
