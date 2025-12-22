-- Wikipedia Dataset - Load and Index
-- Loads Wikipedia articles and creates BM25 index
--
-- Usage:
--   psql -v data_dir="'/path/to/data'" -f load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia Dataset - Data Loading ==='
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

-- Load articles from TSV (convert to CSV to avoid \. escaping issues)
\echo 'Loading Wikipedia articles (this may take several minutes)...'
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

-- Show sample data
\echo ''
\echo 'Sample articles:'
SELECT article_id, title, LEFT(content, 100) || '...' as content_preview
FROM wikipedia_articles
ORDER BY article_id
LIMIT 5;

-- Create BM25 index
\echo ''
\echo '=== Building BM25 Index ==='
\echo 'Creating BM25 index on Wikipedia articles...'
CREATE INDEX wikipedia_bm25_idx ON wikipedia_articles
    USING bm25(content) WITH (text_config='english');

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('wikipedia_bm25_idx')) as index_size,
    pg_relation_size('wikipedia_bm25_idx') as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('wikipedia_articles')) as table_size,
    pg_total_relation_size('wikipedia_articles') as table_bytes;

-- Segment statistics
\echo ''
\echo '=== Segment Statistics ==='
SELECT bm25_summarize_index('wikipedia_bm25_idx');

\echo ''
\echo '=== Wikipedia Load Complete ==='
