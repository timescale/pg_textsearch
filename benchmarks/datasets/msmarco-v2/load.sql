-- MS MARCO v2 Passage Ranking - Load and Index
-- Loads the full 138M passage collection and creates BM25 index
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f load.sql
--
-- The DATA_DIR environment variable should point to the directory containing:
--   - collection.tsv (passage_id, passage_text)
--   - passv2_dev_queries.tsv (query_id, query_text)
--   - passv2_dev_qrels.tsv (query_id, 0, passage_id, relevance)

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO v2 Passage Ranking - Data Loading ==='
\echo 'Loading ~138M passages from MS MARCO v2 collection'
\echo ''

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_v2_passages CASCADE;
DROP TABLE IF EXISTS msmarco_v2_queries CASCADE;
DROP TABLE IF EXISTS msmarco_v2_qrels CASCADE;

-- Create passages table
\echo 'Creating passages table...'
CREATE TABLE msmarco_v2_passages (
    passage_id TEXT PRIMARY KEY,
    passage_text TEXT NOT NULL
);

-- Create queries table
\echo 'Creating queries table...'
CREATE TABLE msmarco_v2_queries (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);

-- Create relevance judgments table
\echo 'Creating relevance judgments table...'
CREATE TABLE msmarco_v2_qrels (
    query_id INTEGER NOT NULL,
    passage_id TEXT NOT NULL,
    relevance INTEGER NOT NULL,
    PRIMARY KEY (query_id, passage_id)
);

-- Load passages (this is the big one - 138M rows)
-- Convert TSV to CSV to avoid issues with \. sequences in text format
\echo 'Loading passages (this will take a long time for 138M rows)...'
\copy msmarco_v2_passages(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \"\\\"\" \$1 \"\\\"\", \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Load queries
\echo 'Loading queries...'
\copy msmarco_v2_queries(query_id, query_text) FROM PROGRAM 'cat "$DATA_DIR/passv2_dev_queries.tsv"' WITH (FORMAT text, DELIMITER E'\t');

-- Load relevance judgments (qrels format: query_id, 0, passage_id, relevance)
\echo 'Loading relevance judgments...'
CREATE TEMP TABLE qrels_raw (
    query_id INTEGER,
    zero INTEGER,
    passage_id TEXT,
    relevance INTEGER
);
\copy qrels_raw FROM PROGRAM 'cat "$DATA_DIR/passv2_dev_qrels.tsv"' WITH (FORMAT text, DELIMITER E'\t');
INSERT INTO msmarco_v2_qrels (query_id, passage_id, relevance)
SELECT query_id, passage_id, relevance FROM qrels_raw;
DROP TABLE qrels_raw;

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT 'Passages loaded:' as metric, COUNT(*)::text as count FROM msmarco_v2_passages
UNION ALL
SELECT 'Queries loaded:', COUNT(*)::text FROM msmarco_v2_queries
UNION ALL
SELECT 'Relevance judgments:', COUNT(*)::text FROM msmarco_v2_qrels
ORDER BY metric;

-- Show sample data
\echo ''
\echo 'Sample passages:'
SELECT passage_id, LEFT(passage_text, 100) || '...' as passage_preview
FROM msmarco_v2_passages
LIMIT 3;

\echo ''
\echo 'Sample queries:'
SELECT query_id, query_text
FROM msmarco_v2_queries
LIMIT 5;

-- Create BM25 index
\echo ''
\echo '=== Building BM25 Index ==='
\echo 'Creating BM25 index on ~138M passages (this will take a long time)...'
CREATE INDEX msmarco_v2_bm25_idx ON msmarco_v2_passages
    USING bm25(passage_text) WITH (text_config='english');

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_v2_bm25_idx')) as index_size,
    pg_relation_size('msmarco_v2_bm25_idx') as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('msmarco_v2_passages')) as table_size,
    pg_total_relation_size('msmarco_v2_passages') as table_bytes;

-- Segment statistics
\echo ''
\echo '=== Segment Statistics ==='
SELECT bm25_summarize_index('msmarco_v2_bm25_idx');

\echo ''
\echo '=== MS MARCO v2 Load Complete ==='
\echo 'Ready for query benchmarks'
