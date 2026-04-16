-- MS MARCO Passage Ranking - VACUUM Performance Benchmark
-- Measures partial vs full VACUUM overhead against a BM25 index
-- with multiple segments.
--
-- Prerequisites: run load.sql first (creates msmarco_passages
-- table and msmarco_bm25_idx index).
--
-- The standard load produces a single merged segment. To test
-- partial vs full VACUUM we first create additional segments
-- via batch inserts with controlled memtable spills, giving
-- us a multi-segment index where dead tuples can be
-- concentrated (partial) or spread everywhere (full).
--
-- Scenarios:
--   Setup: Insert batches → create multi-segment index
--   A. Partial VACUUM — delete from one batch, few segments affected
--   B. Full VACUUM    — delete uniformly, all segments affected
--   C. Full VACUUM    — update uniformly, all segments affected

\set ON_ERROR_STOP on
\timing on

\echo '=== MS MARCO VACUUM Performance Benchmark ==='
\echo ''

-- Disable autovacuum so only our explicit VACUUMs run
ALTER TABLE msmarco_passages SET (autovacuum_enabled = false);

-- ============================================================
-- Helper: run a batch of queries and report average latency
-- ============================================================
CREATE OR REPLACE FUNCTION vacuum_benchmark_queries(
    num_queries int DEFAULT 100
)
RETURNS TABLE(avg_ms numeric, min_ms numeric, max_ms numeric,
              queries_run int) AS $$
DECLARE
    q record;
    start_ts timestamp;
    end_ts timestamp;
    times numeric[];
    sorted_times numeric[];
    n int;
BEGIN
    times := ARRAY[]::numeric[];

    FOR q IN
        SELECT query_text
        FROM msmarco_queries
        ORDER BY query_id
        LIMIT num_queries
    LOOP
        start_ts := clock_timestamp();
        EXECUTE
            'SELECT passage_id FROM msmarco_passages
             ORDER BY passage_text
                 <@> to_bm25query($1, ''msmarco_bm25_idx'')
             LIMIT 10'
            USING q.query_text;
        end_ts := clock_timestamp();
        times := array_append(
            times,
            EXTRACT(EPOCH FROM (end_ts - start_ts)) * 1000
        );
    END LOOP;

    n := array_length(times, 1);
    IF n IS NULL OR n = 0 THEN
        avg_ms := 0;  min_ms := 0;  max_ms := 0;
        queries_run := 0;
        RETURN NEXT;
        RETURN;
    END IF;

    SELECT array_agg(t ORDER BY t)
      INTO sorted_times
      FROM unnest(times) t;

    avg_ms := (SELECT AVG(t) FROM unnest(times) t);
    min_ms := sorted_times[1];
    max_ms := sorted_times[n];
    queries_run := n;
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- ============================================================
-- Setup: Create a multi-segment index
-- ============================================================
--
-- After load.sql the index has a single force-merged segment.
-- We insert 4 batches of ~100K rows each (copied from existing
-- data with offset passage_ids), forcing a memtable spill after
-- each batch via bm25_spill_index.  We suppress auto-spill by
-- raising bulk_load_threshold so that each batch creates
-- exactly one new segment.
--
-- Result: 1 large original segment + 4 smaller L0 segments.

\echo '=== Setup: Creating multi-segment index ==='
\echo ''

\echo 'Initial segment layout:'
SELECT bm25_summarize_index('msmarco_bm25_idx');

-- Suppress auto-spill during batch inserts
SET pg_textsearch.bulk_load_threshold = 2000000000;

-- Batch 1: rows with passage_id offset by 10M
\echo 'Inserting batch 1 (~100K rows)...'
INSERT INTO msmarco_passages (passage_id, passage_text)
SELECT passage_id + 10000000, passage_text
FROM msmarco_passages
WHERE passage_id < 100000;
SELECT bm25_spill_index('msmarco_bm25_idx');

-- Batch 2: offset by 20M
\echo 'Inserting batch 2 (~100K rows)...'
INSERT INTO msmarco_passages (passage_id, passage_text)
SELECT passage_id + 20000000, passage_text
FROM msmarco_passages
WHERE passage_id >= 100000 AND passage_id < 200000;
SELECT bm25_spill_index('msmarco_bm25_idx');

-- Batch 3: offset by 30M
\echo 'Inserting batch 3 (~100K rows)...'
INSERT INTO msmarco_passages (passage_id, passage_text)
SELECT passage_id + 30000000, passage_text
FROM msmarco_passages
WHERE passage_id >= 200000 AND passage_id < 300000;
SELECT bm25_spill_index('msmarco_bm25_idx');

-- Batch 4: offset by 40M
\echo 'Inserting batch 4 (~100K rows)...'
INSERT INTO msmarco_passages (passage_id, passage_text)
SELECT passage_id + 40000000, passage_text
FROM msmarco_passages
WHERE passage_id >= 300000 AND passage_id < 400000;
SELECT bm25_spill_index('msmarco_bm25_idx');

-- Restore default threshold
RESET pg_textsearch.bulk_load_threshold;

\echo ''
\echo 'Segment layout after setup:'
SELECT bm25_summarize_index('msmarco_bm25_idx');

-- ============================================================
-- Baseline measurements
-- ============================================================
\echo ''
\echo '=== Baseline ==='

SELECT
    'VACUUM_TOTAL_ROWS:' as label,
    COUNT(*) as rows
FROM msmarco_passages;

SELECT
    'VACUUM_BASELINE_INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx'))
        as index_size,
    pg_relation_size('msmarco_bm25_idx')
        as index_bytes;

SELECT
    'VACUUM_BASELINE_TABLE_SIZE:' as label,
    pg_size_pretty(pg_total_relation_size('msmarco_passages'))
        as table_size,
    pg_total_relation_size('msmarco_passages')
        as table_bytes;

-- Warm up
\echo 'Warming up queries...'
SELECT * FROM vacuum_benchmark_queries(20);

\echo 'Baseline query throughput (100 queries):'
SELECT
    'VACUUM_BASELINE_QUERY: avg='
        || round(avg_ms, 2) || 'ms min='
        || round(min_ms, 2) || 'ms max='
        || round(max_ms, 2) || 'ms (n='
        || queries_run || ')' as result
FROM vacuum_benchmark_queries(100);

-- ============================================================
-- Scenario A: Partial VACUUM — concentrated delete
-- ============================================================
-- Delete all ~100K rows from batch 1 (passage_id 10M..10.1M).
-- Only the segment containing batch 1 data has dead tuples;
-- the large original segment and other batch segments are
-- unaffected.  VACUUM should skip unaffected segments.

\echo ''
\echo '=== Scenario A: Partial VACUUM (concentrated delete) ==='

\echo 'Deleting batch 1 rows (~100K, passage_id 10M..10.1M)...'
\echo 'VACUUM_PARTIAL_DELETE_START:'
DELETE FROM msmarco_passages
WHERE passage_id >= 10000000 AND passage_id < 10100000;
\echo 'VACUUM_PARTIAL_DELETE_END:'

SELECT
    'VACUUM_PARTIAL_ROWS_DELETED:' as label,
    (SELECT COUNT(*) FROM msmarco_passages) as remaining;

SELECT
    'VACUUM_PARTIAL_PRE_INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx'))
        as index_size,
    pg_relation_size('msmarco_bm25_idx')
        as index_bytes;

\echo 'Running VACUUM (partial — few segments affected)...'
\echo 'VACUUM_PARTIAL_START:'
VACUUM msmarco_passages;
\echo 'VACUUM_PARTIAL_END:'

SELECT
    'VACUUM_PARTIAL_POST_INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx'))
        as index_size,
    pg_relation_size('msmarco_bm25_idx')
        as index_bytes;

\echo 'Post-partial-VACUUM query throughput (100 queries):'
SELECT
    'VACUUM_PARTIAL_QUERY: avg='
        || round(avg_ms, 2) || 'ms min='
        || round(min_ms, 2) || 'ms max='
        || round(max_ms, 2) || 'ms (n='
        || queries_run || ')' as result
FROM vacuum_benchmark_queries(100);

\echo ''
\echo 'Segment layout after partial VACUUM:'
SELECT bm25_summarize_index('msmarco_bm25_idx');

-- ============================================================
-- Scenario B: Full VACUUM — uniform delete
-- ============================================================
-- Delete ~100K rows uniformly across the entire dataset using
-- a hash function so dead tuples land in every segment.
-- VACUUM must process all segments.

\echo ''
\echo '=== Scenario B: Full VACUUM (uniform delete) ==='

\echo 'Deleting ~100K rows uniformly (hashint4 % N)...'

-- Calculate modulus to delete roughly 100K of remaining rows
DO $$
DECLARE
    total_rows bigint;
    target_deletes int := 100000;
    modulus int;
BEGIN
    SELECT COUNT(*) INTO total_rows FROM msmarco_passages;
    modulus := greatest(total_rows / target_deletes, 2);
    RAISE NOTICE 'Total rows: %, modulus: % (target ~% deletes)',
        total_rows, modulus, total_rows / modulus;
    -- Store modulus for the DELETE below
    PERFORM set_config('vacuum_bench.modulus',
                       modulus::text, false);
END;
$$;

\echo 'VACUUM_FULL_DELETE_START:'
DELETE FROM msmarco_passages
WHERE hashint4(passage_id) %
      current_setting('vacuum_bench.modulus')::int = 0;
\echo 'VACUUM_FULL_DELETE_END:'

SELECT
    'VACUUM_FULL_DELETE_ROWS_REMAINING:' as label,
    COUNT(*) as remaining
FROM msmarco_passages;

SELECT
    'VACUUM_FULL_DELETE_PRE_INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx'))
        as index_size,
    pg_relation_size('msmarco_bm25_idx')
        as index_bytes;

\echo 'Running VACUUM (full — all segments affected)...'
\echo 'VACUUM_FULL_START:'
VACUUM msmarco_passages;
\echo 'VACUUM_FULL_END:'

SELECT
    'VACUUM_FULL_POST_INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx'))
        as index_size,
    pg_relation_size('msmarco_bm25_idx')
        as index_bytes;

\echo 'Post-full-VACUUM query throughput (100 queries):'
SELECT
    'VACUUM_FULL_QUERY: avg='
        || round(avg_ms, 2) || 'ms min='
        || round(min_ms, 2) || 'ms max='
        || round(max_ms, 2) || 'ms (n='
        || queries_run || ')' as result
FROM vacuum_benchmark_queries(100);

\echo ''
\echo 'Segment layout after full VACUUM:'
SELECT bm25_summarize_index('msmarco_bm25_idx');

-- ============================================================
-- Scenario C: Full VACUUM — uniform update
-- ============================================================
-- Update ~100K rows uniformly.  Each update creates a new
-- heap tuple version and dead-marks the old one.  The BM25
-- index has entries for the old CTIDs that VACUUM must clean
-- up across all segments.

\echo ''
\echo '=== Scenario C: Full VACUUM (uniform update) ==='

\echo 'Updating ~100K rows uniformly...'

\echo 'VACUUM_UPDATE_START:'
UPDATE msmarco_passages
SET passage_text = passage_text || ' (revised)'
WHERE hashint4(passage_id) %
      current_setting('vacuum_bench.modulus')::int = 1;
\echo 'VACUUM_UPDATE_END:'

SELECT
    'VACUUM_UPDATE_PRE_INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx'))
        as index_size,
    pg_relation_size('msmarco_bm25_idx')
        as index_bytes;

\echo 'Running VACUUM (full — all segments affected by update)...'
\echo 'VACUUM_UPDATE_VACUUM_START:'
VACUUM msmarco_passages;
\echo 'VACUUM_UPDATE_VACUUM_END:'

SELECT
    'VACUUM_UPDATE_POST_INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx'))
        as index_size,
    pg_relation_size('msmarco_bm25_idx')
        as index_bytes;

\echo 'Post-update-VACUUM query throughput (100 queries):'
SELECT
    'VACUUM_UPDATE_QUERY: avg='
        || round(avg_ms, 2) || 'ms min='
        || round(min_ms, 2) || 'ms max='
        || round(max_ms, 2) || 'ms (n='
        || queries_run || ')' as result
FROM vacuum_benchmark_queries(100);

\echo ''
\echo 'Segment layout after update VACUUM:'
SELECT bm25_summarize_index('msmarco_bm25_idx');

-- ============================================================
-- Summary
-- ============================================================
\echo ''
\echo '=== VACUUM Benchmark Summary ==='

SELECT
    'VACUUM_FINAL_ROW_COUNT:' as label,
    COUNT(*) as rows
FROM msmarco_passages;

SELECT
    'VACUUM_FINAL_INDEX_SIZE:' as label,
    pg_size_pretty(pg_relation_size('msmarco_bm25_idx'))
        as index_size,
    pg_relation_size('msmarco_bm25_idx')
        as index_bytes;

-- Cleanup: re-enable autovacuum first in case later statements fail
ALTER TABLE msmarco_passages SET (autovacuum_enabled = true);
DROP FUNCTION IF EXISTS vacuum_benchmark_queries;

\echo ''
\echo '=== VACUUM Benchmark Complete ==='
