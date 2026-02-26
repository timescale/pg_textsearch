-- MS MARCO Passage Ranking - Concurrent Insert Setup
-- Loads passages into a staging table, creates empty target table
-- with BM25 index, and prepares a sequence for pgbench-driven
-- concurrent inserts.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f insert/concurrent-setup.sql
--
-- After running this script, use pgbench with pgbench-insert.sql
-- to drive concurrent inserts from staging into the indexed table.

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO Concurrent Insert Setup ==='
\echo ''

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_passages CASCADE;
DROP TABLE IF EXISTS msmarco_queries CASCADE;
DROP TABLE IF EXISTS msmarco_qrels CASCADE;
DROP TABLE IF EXISTS msmarco_staging CASCADE;
DROP SEQUENCE IF EXISTS insert_seq;

-- Create staging table
\echo 'Creating staging table...'
CREATE TABLE msmarco_staging (
    staging_id SERIAL PRIMARY KEY,
    passage_id INTEGER,
    passage_text TEXT
);

-- Load passages into staging
\echo 'Loading passages into staging table...'
\copy msmarco_staging(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \$1, \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Create target passages table
\echo 'Creating target passages table...'
CREATE TABLE msmarco_passages (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL
);

-- Create BM25 index on empty target table
\echo 'Creating BM25 index on empty target table...'
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

-- Create sequence for pgbench to draw row IDs from
\echo 'Creating insert sequence...'
CREATE SEQUENCE insert_seq;

-- Report staging row count
\echo ''
\echo '=== Setup Verification ==='
SELECT 'STAGING_ROW_COUNT:' as label,
    COUNT(*)::text as count
FROM msmarco_staging;

\echo ''
\echo '=== Concurrent Insert Setup Complete ==='
\echo 'Run pgbench with pgbench-insert.sql to start inserts'
