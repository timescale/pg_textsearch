-- Wikipedia Dataset - Load and Index (System X)
-- Loads Wikipedia articles and creates System X BM25 index
--
-- Usage:
--   psql -v data_dir="'/path/to/data'" -f load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia Dataset - Data Loading (System X) ==='
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

-- Load articles from TSV (convert to CSV to avoid \. escaping issues)
\echo 'Loading Wikipedia articles (this may take several minutes)...'
\copy wikipedia_articles_systemx(article_id, title, content) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); gsub(/\"/, \"\\\"\\\"\", \$3); print \$1, \"\\\"\" \$2 \"\\\"\", \"\\\"\" \$3 \"\\\"\"}" "$DATA_DIR/wikipedia.tsv"' WITH (FORMAT csv);

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
    ROUND(SUM(LENGTH(content)) / 1024.0 / 1024.0, 1)::text
FROM wikipedia_articles_systemx;

-- Show sample data
\echo ''
\echo 'Sample articles:'
SELECT article_id, title, LEFT(content, 100) || '...' as content_preview
FROM wikipedia_articles_systemx
ORDER BY article_id
LIMIT 5;

-- Create System X BM25 index with English tokenizer
\echo ''
\echo '=== Building System X BM25 Index ==='
\echo 'Creating System X BM25 index on Wikipedia articles...'
CREATE INDEX wikipedia_systemx_idx ON wikipedia_articles_systemx
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

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('wikipedia_systemx_idx')) as index_size,
    pg_relation_size('wikipedia_systemx_idx') as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('wikipedia_articles_systemx')) as table_size,
    pg_total_relation_size('wikipedia_articles_systemx') as table_bytes;

\echo ''
\echo '=== Wikipedia Load Complete (System X) ==='
