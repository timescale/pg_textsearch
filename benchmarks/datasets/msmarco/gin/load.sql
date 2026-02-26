-- MS MARCO GIN+tsvector Baseline - Data Loading
-- Loads data first, then creates GIN index on populated table
-- Measures CREATE INDEX time and final sizes
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f gin/load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO GIN Baseline - Data Loading ==='
\echo ''

-- Clean up existing tables
DROP TABLE IF EXISTS msmarco_gin_passages CASCADE;

-- Create passages table (without index)
\echo 'Creating passages table...'
CREATE TABLE msmarco_gin_passages (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL,
    tsv tsvector GENERATED ALWAYS AS (
        to_tsvector('english', passage_text)
    ) STORED
);

-- Load passages
\echo 'Loading passages...'
\copy msmarco_gin_passages(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \$1, \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Create GIN index on populated table (timed)
\echo 'Building GIN index...'
CREATE INDEX msmarco_gin_idx
    ON msmarco_gin_passages USING gin(tsv);

-- Report index and table sizes
\echo ''
\echo '=== Index Size Report ==='
SELECT
    'INDEX_SIZE:' as label,
    pg_size_pretty(
        pg_relation_size('msmarco_gin_idx')
    ) as index_size,
    pg_relation_size('msmarco_gin_idx')
        as index_bytes;
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(
        pg_total_relation_size('msmarco_gin_passages')
    ) as table_size,
    pg_total_relation_size('msmarco_gin_passages')
        as table_bytes;

\echo ''
\echo '=== MS MARCO GIN Load Complete ==='
