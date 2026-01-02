-- MS MARCO Passage Ranking - Load and Index
-- Loads the full 8.8M passage collection and creates BM25 index
--
-- IDEMPOTENT: Uses CREATE INDEX IF NOT EXISTS to skip index rebuild.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO Passage Ranking - Data Loading ==='
\echo ''

-- Drop and recreate tables for clean state
DROP TABLE IF EXISTS msmarco_passages CASCADE;
DROP TABLE IF EXISTS msmarco_queries CASCADE;
DROP TABLE IF EXISTS msmarco_qrels CASCADE;

-- Create passages table
\echo 'Creating passages table...'
CREATE TABLE msmarco_passages (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL
);

-- Create queries table
\echo 'Creating queries table...'
CREATE TABLE msmarco_queries (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);

-- Create relevance judgments table
\echo 'Creating relevance judgments table...'
CREATE TABLE msmarco_qrels (
    query_id INTEGER NOT NULL,
    passage_id INTEGER NOT NULL,
    relevance INTEGER NOT NULL,
    PRIMARY KEY (query_id, passage_id)
);

-- Load passages (this is the big one - 8.8M rows)
-- Convert TSV to CSV to avoid issues with \. sequences in text format
\echo 'Loading passages (this may take several minutes)...'
\copy msmarco_passages(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", $2); print $1, \"\\\"\" $2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv)

-- Load all dev queries (6,980 queries with relevance judgments)
-- Use CSV format to avoid backslash escape issues in text format
\echo 'Loading queries...'
\copy msmarco_queries(query_id, query_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", $2); print $1, \"\\\"\" $2 \"\\\"\"}" "$DATA_DIR/queries.dev.tsv"' WITH (FORMAT csv)

-- Load relevance judgments (qrels format: query_id, 0, passage_id, relevance)
\echo 'Loading relevance judgments...'
CREATE TEMP TABLE qrels_raw (
    query_id INTEGER,
    zero INTEGER,
    passage_id INTEGER,
    relevance INTEGER
);
\copy qrels_raw FROM PROGRAM 'cat "$DATA_DIR/qrels.dev.small.tsv"' WITH (FORMAT text, DELIMITER E'\t')
INSERT INTO msmarco_qrels (query_id, passage_id, relevance)
SELECT query_id, passage_id, relevance FROM qrels_raw;
DROP TABLE qrels_raw;

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT 'Passages loaded:' as metric, COUNT(*)::text as count FROM msmarco_passages
UNION ALL
SELECT 'Queries loaded:', COUNT(*)::text FROM msmarco_queries
UNION ALL
SELECT 'Relevance judgments:', COUNT(*)::text FROM msmarco_qrels
ORDER BY metric;

-- Show sample data
\echo ''
\echo 'Sample passages:'
SELECT passage_id, LEFT(passage_text, 100) || '...' as passage_preview
FROM msmarco_passages
LIMIT 3;

\echo ''
\echo 'Sample queries:'
SELECT query_id, query_text
FROM msmarco_queries
LIMIT 5;

-- Create BM25 index
\echo ''
\echo '=== Building BM25 Index ==='
\echo 'Creating BM25 index on passages (this will take a while)...'
CREATE INDEX msmarco_bm25_idx ON msmarco_passages
    USING bm25(passage_text) WITH (text_config='english');

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx')) as index_size,
    pg_relation_size('msmarco_bm25_idx') as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('msmarco_passages')) as table_size,
    pg_total_relation_size('msmarco_passages') as table_bytes;

-- Segment statistics
\echo ''
\echo '=== Segment Statistics ==='
SELECT bm25_summarize_index('msmarco_bm25_idx');

\echo ''
\echo '=== MS MARCO Load Complete ==='
\echo 'Ready for query benchmarks'
