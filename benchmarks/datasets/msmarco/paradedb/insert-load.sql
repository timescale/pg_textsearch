-- MS MARCO Passage Ranking - ParadeDB Insert Loading
-- Creates ParadeDB BM25 index on EMPTY table, then loads data
-- via COPY to measure insert-path indexing performance.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f paradedb/insert-load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO ParadeDB Insert Benchmark - Data Loading ==='
\echo 'Index created BEFORE loading ~8.8M passages'
\echo ''

-- Ensure ParadeDB extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_passages_paradedb CASCADE;
DROP TABLE IF EXISTS msmarco_queries_paradedb CASCADE;
DROP TABLE IF EXISTS msmarco_qrels_paradedb CASCADE;

-- Create passages table
\echo 'Creating passages table...'
CREATE TABLE msmarco_passages_paradedb (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL
);

-- Create ParadeDB BM25 index on the empty table
\echo 'Creating ParadeDB BM25 index on empty table...'
CREATE INDEX msmarco_paradedb_idx ON msmarco_passages_paradedb
    USING bm25 (passage_id, passage_text)
    WITH (
        key_field = 'passage_id',
        text_fields = '{
            "passage_text": {
                "tokenizer": {
                    "type": "default",
                    "stopwords_language": "English",
                    "stemmer": "English"
                }
            }
        }'
    );

-- Create queries table
\echo 'Creating queries table...'
CREATE TABLE msmarco_queries_paradedb (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);

-- Create relevance judgments table
\echo 'Creating relevance judgments table...'
CREATE TABLE msmarco_qrels_paradedb (
    query_id INTEGER NOT NULL,
    passage_id INTEGER NOT NULL,
    relevance INTEGER NOT NULL,
    PRIMARY KEY (query_id, passage_id)
);

-- Load passages into the pre-indexed table
\echo ''
\echo 'INSERT_TIME:'
\echo 'Loading passages (this may take a while)...'
\copy msmarco_passages_paradedb(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \$1, \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Load queries
\echo 'Loading queries...'
\copy msmarco_queries_paradedb(query_id, query_text) FROM PROGRAM 'head -10000 "$DATA_DIR/queries.dev.tsv"' WITH (FORMAT text, DELIMITER E'\t');

-- Load relevance judgments
\echo 'Loading relevance judgments...'
CREATE TEMP TABLE qrels_raw (
    query_id INTEGER,
    zero INTEGER,
    passage_id INTEGER,
    relevance INTEGER
);
\copy qrels_raw FROM PROGRAM 'cat "$DATA_DIR/qrels.dev.small.tsv"' WITH (FORMAT text, DELIMITER E'\t');
INSERT INTO msmarco_qrels_paradedb
    (query_id, passage_id, relevance)
SELECT query_id, passage_id, relevance FROM qrels_raw;
DROP TABLE qrels_raw;

-- Compact index segments (ParadeDB merges during VACUUM)
\echo 'INDEX_VACUUM:'
VACUUM msmarco_passages_paradedb;

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT 'Passages loaded:' as metric,
    COUNT(*)::text as count
    FROM msmarco_passages_paradedb
UNION ALL
SELECT 'Queries loaded:', COUNT(*)::text
    FROM msmarco_queries_paradedb
UNION ALL
SELECT 'Relevance judgments:', COUNT(*)::text
    FROM msmarco_qrels_paradedb
ORDER BY metric;

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('msmarco_paradedb_idx')
    ) as index_size,
    pg_relation_size('msmarco_paradedb_idx')
        as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size(
            'msmarco_passages_paradedb')
    ) as table_size,
    pg_total_relation_size(
        'msmarco_passages_paradedb') as table_bytes;

\echo ''
\echo '=== MS MARCO ParadeDB Insert Benchmark Complete ==='
\echo 'Ready for query benchmarks'
