-- Wikipedia GIN+tsvector Baseline - Insert Loading
-- Creates GIN index on EMPTY table, then loads data via COPY
-- Measures insert performance into an already-indexed table
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f gin/insert-load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia GIN Insert Benchmark - Data Loading ==='
\echo 'Index created BEFORE loading articles'
\echo ''

-- Clean up existing tables
DROP TABLE IF EXISTS wikipedia_gin_articles CASCADE;

-- Create articles table
\echo 'Creating articles table...'
CREATE TABLE wikipedia_gin_articles (
    article_id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL,
    tsv tsvector GENERATED ALWAYS AS (
        to_tsvector('english', content)
    ) STORED
);

-- Create GIN index on empty table
\echo 'Creating GIN index on empty table...'
CREATE INDEX wikipedia_gin_idx
    ON wikipedia_gin_articles USING gin(tsv);

-- Load articles into the pre-indexed table
\echo ''
\echo 'INSERT_TIME:'
\echo 'Loading articles into indexed table...'
\copy wikipedia_gin_articles(article_id, title, content) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); gsub(/\"/, \"\\\"\\\"\", \$3); print \$1, \"\\\"\" \$2 \"\\\"\", \"\\\"\" \$3 \"\\\"\"}" "$DATA_DIR/wikipedia.tsv"' WITH (FORMAT csv);

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('wikipedia_gin_idx')
    ) as index_size,
    pg_relation_size('wikipedia_gin_idx')
        as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size(
            'wikipedia_gin_articles')
    ) as table_size,
    pg_total_relation_size(
        'wikipedia_gin_articles') as table_bytes;

\echo ''
\echo '=== Wikipedia GIN Insert Benchmark Complete ==='
