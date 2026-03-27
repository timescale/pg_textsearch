-- MS MARCO Passage Ranking - System X Insert Loading
-- Creates System X BM25 index on EMPTY table, then loads data
-- via COPY to measure insert-path indexing performance.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f systemx/insert-load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO System X Insert Benchmark - Data Loading ==='
\echo 'Index created BEFORE loading ~8.8M passages'
\echo ''

-- Ensure System X extension is installed
CREATE EXTENSION IF NOT EXISTS pg_search;

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_passages_systemx CASCADE;
DROP TABLE IF EXISTS msmarco_queries_systemx CASCADE;
DROP TABLE IF EXISTS msmarco_qrels_systemx CASCADE;

-- Create passages table
\echo 'Creating passages table...'
CREATE TABLE msmarco_passages_systemx (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL
);

-- Create System X BM25 index on the empty table
\echo 'Creating System X BM25 index on empty table...'
CREATE INDEX msmarco_systemx_idx ON msmarco_passages_systemx
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
CREATE TABLE msmarco_queries_systemx (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);

-- Create relevance judgments table
\echo 'Creating relevance judgments table...'
CREATE TABLE msmarco_qrels_systemx (
    query_id INTEGER NOT NULL,
    passage_id INTEGER NOT NULL,
    relevance INTEGER NOT NULL,
    PRIMARY KEY (query_id, passage_id)
);

-- Load passages into the pre-indexed table
\echo ''
\echo 'INSERT_TIME:'
\echo 'Loading passages (this may take a while)...'
\copy msmarco_passages_systemx(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \$1, \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Load queries
\echo 'Loading queries...'
\copy msmarco_queries_systemx(query_id, query_text) FROM PROGRAM 'head -10000 "$DATA_DIR/queries.dev.tsv"' WITH (FORMAT text, DELIMITER E'\t');

-- Load relevance judgments
\echo 'Loading relevance judgments...'
CREATE TEMP TABLE qrels_raw (
    query_id INTEGER,
    zero INTEGER,
    passage_id INTEGER,
    relevance INTEGER
);
\copy qrels_raw FROM PROGRAM 'cat "$DATA_DIR/qrels.dev.small.tsv"' WITH (FORMAT text, DELIMITER E'\t');
INSERT INTO msmarco_qrels_systemx
    (query_id, passage_id, relevance)
SELECT query_id, passage_id, relevance FROM qrels_raw;
DROP TABLE qrels_raw;

-- Compact index segments (System X merges during VACUUM)
\echo 'INDEX_VACUUM:'
VACUUM msmarco_passages_systemx;

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT 'Passages loaded:' as metric,
    COUNT(*)::text as count
    FROM msmarco_passages_systemx
UNION ALL
SELECT 'Queries loaded:', COUNT(*)::text
    FROM msmarco_queries_systemx
UNION ALL
SELECT 'Relevance judgments:', COUNT(*)::text
    FROM msmarco_qrels_systemx
ORDER BY metric;

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('msmarco_systemx_idx')
    ) as index_size,
    pg_relation_size('msmarco_systemx_idx')
        as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size(
            'msmarco_passages_systemx')
    ) as table_size,
    pg_total_relation_size(
        'msmarco_passages_systemx') as table_bytes;

\echo ''
\echo '=== MS MARCO System X Insert Benchmark Complete ==='
\echo 'Ready for query benchmarks'
