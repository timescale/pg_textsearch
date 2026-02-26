-- MS MARCO Passage Ranking - Insert Benchmark (Single Transaction)
-- Creates BM25 index on an empty table, then loads data via COPY
-- to measure insert-path indexing performance.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f insert/load.sql
--
-- The DATA_DIR environment variable should point to the directory
-- containing:
--   - collection.tsv (passage_id, passage_text)
--   - queries.dev.tsv (query_id, query_text)
--   - qrels.dev.small.tsv (query_id, 0, passage_id, relevance)

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO Insert Benchmark - Data Loading ==='
\echo 'Index created BEFORE loading ~8.8M passages'
\echo ''

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_passages CASCADE;
DROP TABLE IF EXISTS msmarco_queries CASCADE;
DROP TABLE IF EXISTS msmarco_qrels CASCADE;

-- Create passages table
\echo 'Creating passages table...'
CREATE TABLE msmarco_passages (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL
);

-- Create BM25 index on the empty table
\echo 'Creating BM25 index on empty table...'
CREATE INDEX msmarco_bm25_idx ON msmarco_passages
    USING bm25(passage_text) WITH (text_config='english');

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

-- Load passages into the pre-indexed table
\echo ''
\echo 'INSERT_TIME:'
\echo 'Loading passages (this may take a while with live index)...'
\copy msmarco_passages(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \$1, \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Load queries
\echo 'Loading queries...'
\copy msmarco_queries(query_id, query_text) FROM PROGRAM 'head -10000 "$DATA_DIR/queries.dev.tsv"' WITH (FORMAT text, DELIMITER E'\t');

-- Load relevance judgments
\echo 'Loading relevance judgments...'
CREATE TEMP TABLE qrels_raw (
    query_id INTEGER,
    zero INTEGER,
    passage_id INTEGER,
    relevance INTEGER
);
\copy qrels_raw FROM PROGRAM 'cat "$DATA_DIR/qrels.dev.small.tsv"' WITH (FORMAT text, DELIMITER E'\t');
INSERT INTO msmarco_qrels (query_id, passage_id, relevance)
SELECT query_id, passage_id, relevance FROM qrels_raw;
DROP TABLE qrels_raw;

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT 'Passages loaded:' as metric,
    COUNT(*)::text as count FROM msmarco_passages
UNION ALL
SELECT 'Queries loaded:', COUNT(*)::text
    FROM msmarco_queries
UNION ALL
SELECT 'Relevance judgments:', COUNT(*)::text
    FROM msmarco_qrels
ORDER BY metric;

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('msmarco_bm25_idx')
    ) as index_size,
    pg_relation_size('msmarco_bm25_idx') as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size('msmarco_passages')
    ) as table_size,
    pg_total_relation_size('msmarco_passages')
        as table_bytes;

-- Segment statistics
\echo ''
\echo '=== Segment Statistics ==='
SELECT bm25_summarize_index('msmarco_bm25_idx');

\echo ''
\echo '=== MS MARCO Insert Benchmark Load Complete ==='
\echo 'Ready for query benchmarks'
