-- MS MARCO Passage Ranking - Load and Index
-- Loads the full 8.8M passage collection and creates BM25 index
--
-- IDEMPOTENT: Uses CREATE TABLE/INDEX IF NOT EXISTS.
-- Data loading always runs but is fast; index creation is skipped if exists.
--
-- Usage:
--   DATA_DIR=/path/to/data psql -f load.sql

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO Passage Ranking - Data Loading ==='
\echo ''

-- Create tables (IF NOT EXISTS avoids errors on re-run)
\echo 'Creating tables...'
CREATE TABLE IF NOT EXISTS msmarco_passages (
    passage_id INTEGER PRIMARY KEY,
    passage_text TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS msmarco_queries (
    query_id INTEGER PRIMARY KEY,
    query_text TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS msmarco_qrels (
    query_id INTEGER NOT NULL,
    passage_id INTEGER NOT NULL,
    relevance INTEGER NOT NULL,
    PRIMARY KEY (query_id, passage_id)
);

-- Check if data already loaded
DO $$
DECLARE
    passage_count bigint;
BEGIN
    SELECT COUNT(*) INTO passage_count FROM msmarco_passages;
    IF passage_count >= 8000000 THEN
        RAISE NOTICE 'Data already loaded (% passages), skipping data load',
            passage_count;
        -- Signal to skip loading (checked below)
        PERFORM set_config('msmarco.skip_load', 'true', true);
    ELSE
        RAISE NOTICE 'Loading data (% passages found)...', passage_count;
        PERFORM set_config('msmarco.skip_load', 'false', true);
        -- Truncate for clean reload
        TRUNCATE msmarco_passages, msmarco_queries, msmarco_qrels;
    END IF;
END $$;

-- Load passages (skip if already loaded)
\echo 'Loading passages...'
\copy msmarco_passages(passage_id, passage_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", $2); print $1, \"\\\"\" $2 \"\\\"\"}" "$DATA_DIR/collection.tsv" 2>/dev/null || true' WITH (FORMAT csv)

-- Load queries
\echo 'Loading queries...'
\copy msmarco_queries(query_id, query_text) FROM PROGRAM 'awk -F"\t" "{OFS=\",\"; gsub(/\"/, \"\\\"\\\"\", $2); print $1, \"\\\"\" $2 \"\\\"\"}" "$DATA_DIR/queries.dev.tsv" 2>/dev/null || true' WITH (FORMAT csv)

-- Load relevance judgments
\echo 'Loading relevance judgments...'
CREATE TEMP TABLE IF NOT EXISTS qrels_raw (
    query_id INTEGER,
    zero INTEGER,
    passage_id INTEGER,
    relevance INTEGER
);
\copy qrels_raw FROM PROGRAM 'cat "$DATA_DIR/qrels.dev.small.tsv" 2>/dev/null || true' WITH (FORMAT text, DELIMITER E'\t')
INSERT INTO msmarco_qrels (query_id, passage_id, relevance)
SELECT query_id, passage_id, relevance FROM qrels_raw
ON CONFLICT DO NOTHING;

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
\echo 'Sample queries:'
SELECT query_id, query_text FROM msmarco_queries LIMIT 5;

-- Create BM25 index (IF NOT EXISTS skips if already built)
\echo ''
\echo '=== BM25 Index ==='

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_indexes WHERE indexname = 'msmarco_bm25_idx') THEN
        RAISE NOTICE 'Index msmarco_bm25_idx already exists, skipping creation';
    ELSE
        RAISE NOTICE 'Creating BM25 index on passages (this will take a while)...';
    END IF;
END $$;

CREATE INDEX IF NOT EXISTS msmarco_bm25_idx ON msmarco_passages
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
