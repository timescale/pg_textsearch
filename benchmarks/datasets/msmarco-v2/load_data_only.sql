-- MS MARCO v2 Passage Ranking - Data Loading Only (no index)
-- Loads the full 138M passage collection without creating the index
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f load_data_only.sql
--
-- After loading, run parallel_scaling.sql to benchmark different worker counts

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO v2 Passage Ranking - Data Loading ==='
\echo 'Loading ~138M passages from MS MARCO v2 collection (no index)'
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
\echo 'Loading passages (this will take a long time for 138M rows)...'
\copy msmarco_v2_passages(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", \$2); print \"\\\"\" \$1 \"\\\"\", \"\\\"\" \$2 \"\\\"\"}" "$DATA_DIR/collection.tsv"' WITH (FORMAT csv);

-- Load queries
\echo 'Loading queries...'
\copy msmarco_v2_queries(query_id, query_text) FROM PROGRAM 'cat "$DATA_DIR/passv2_dev_queries.tsv"' WITH (FORMAT text, DELIMITER E'\t');

-- Load relevance judgments
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

-- Analyze for accurate statistics
\echo 'Analyzing tables...'
ANALYZE msmarco_v2_passages;
ANALYZE msmarco_v2_queries;
ANALYZE msmarco_v2_qrels;

-- Verify data loading
\echo ''
\echo '=== Data Loading Verification ==='
SELECT 'Passages loaded:' as metric, COUNT(*)::text as count FROM msmarco_v2_passages
UNION ALL
SELECT 'Queries loaded:', COUNT(*)::text FROM msmarco_v2_queries
UNION ALL
SELECT 'Relevance judgments:', COUNT(*)::text FROM msmarco_v2_qrels
ORDER BY metric;

-- Report table size
\echo ''
\echo '=== Table Size ==='
SELECT
    'TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('msmarco_v2_passages')) as table_size,
    pg_total_relation_size('msmarco_v2_passages') as table_bytes;

\echo ''
\echo '=== Data Loading Complete ==='
\echo 'Run parallel_scaling.sql to benchmark different worker counts'
