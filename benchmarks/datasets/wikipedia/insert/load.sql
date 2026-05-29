-- Wikipedia Dataset - Insert Benchmark (Single Transaction)
-- Creates BM25 index on an empty table, then loads data via COPY
-- to measure insert-path indexing performance.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f insert/load.sql
--
-- The DATA_DIR environment variable should point to the directory
-- containing:
--   - wikipedia.tsv (article_id, title, content)

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia Insert Benchmark - Data Loading ==='
\echo 'Index created BEFORE loading articles'
\echo ''

-- Clean up existing tables
DROP TABLE IF EXISTS wikipedia_articles CASCADE;

-- Create articles table
\echo 'Creating articles table...'
CREATE TABLE wikipedia_articles (
    article_id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    content TEXT NOT NULL
);

-- Create BM25 index on the empty table
\echo 'Creating BM25 index on empty table...'
CREATE INDEX wikipedia_bm25_idx ON wikipedia_articles
    USING bm25(content) WITH (text_config='english');

-- Load articles into the pre-indexed table
\echo ''
\echo 'INSERT_TIME:'
\echo 'Loading articles (this may take a while with live index)...'
\copy wikipedia_articles(article_id, title, content) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); gsub(/\"/, \"\\\"\\\"\", \$3); print \$1, \"\\\"\" \$2 \"\\\"\", \"\\\"\" \$3 \"\\\"\"}" "$DATA_DIR/wikipedia.tsv"' WITH (FORMAT csv);

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT
    'Articles loaded' as metric,
    COUNT(*)::text as value
FROM wikipedia_articles
UNION ALL
SELECT
    'Avg content length (chars)',
    ROUND(AVG(LENGTH(content)))::text
FROM wikipedia_articles
UNION ALL
SELECT
    'Total text size (MB)',
    ROUND(SUM(LENGTH(content)) / 1024.0 / 1024.0, 1)::text
FROM wikipedia_articles;

-- NOTE: spill + size + segment-stats moved to size_report.sql, run
-- AFTER queries.sql, so the query phase exercises the realistic
-- post-load state (in-flight memtable chain + cache + segments).

\echo ''
\echo '=== Wikipedia Insert Benchmark Load Complete ==='
\echo 'Ready for query benchmarks'
