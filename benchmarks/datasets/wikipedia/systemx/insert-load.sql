-- Wikipedia Dataset - System X Insert Loading
-- Creates System X BM25 index on EMPTY table, then loads data
-- via COPY to measure insert-path indexing performance.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f systemx/insert-load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia System X Insert Benchmark - Data Loading ==='
\echo 'Index created BEFORE loading articles'
\echo ''

-- Ensure System X extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up existing tables
DROP TABLE IF EXISTS wikipedia_articles_systemx CASCADE;

-- Create articles table
\echo 'Creating articles table...'
CREATE TABLE wikipedia_articles_systemx (
    article_id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL
);

-- Create System X BM25 index on the empty table
\echo 'Creating System X BM25 index on empty table...'
CREATE INDEX wikipedia_systemx_idx
    ON wikipedia_articles_systemx
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
\copy wikipedia_articles_systemx(article_id, title, content) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); gsub(/\"/, \"\\\"\\\"\", \$3); print \$1, \"\\\"\" \$2 \"\\\"\", \"\\\"\" \$3 \"\\\"\"}" "$DATA_DIR/wikipedia.tsv"' WITH (FORMAT csv);

-- Compact index segments (System X merges during VACUUM)
\echo 'INDEX_VACUUM:'
VACUUM wikipedia_articles_systemx;

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT
    'Articles loaded' as metric,
    COUNT(*)::text as value
FROM wikipedia_articles_systemx
UNION ALL
SELECT
    'Avg content length (chars)',
    ROUND(AVG(LENGTH(content)))::text
FROM wikipedia_articles_systemx
UNION ALL
SELECT
    'Total text size (MB)',
    ROUND(
        SUM(LENGTH(content)) / 1024.0 / 1024.0,
        1
    )::text
FROM wikipedia_articles_systemx;

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('wikipedia_systemx_idx')
    ) as index_size,
    pg_relation_size('wikipedia_systemx_idx')
        as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size(
            'wikipedia_articles_systemx')
    ) as table_size,
    pg_total_relation_size(
        'wikipedia_articles_systemx') as table_bytes;

\echo ''
\echo '=== Wikipedia System X Insert Benchmark Complete ==='
\echo 'Ready for query benchmarks'
