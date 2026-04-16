-- Test pg_textsearch.memory_limit GUC
-- Derives per-index (limit/8) and global (limit/2) soft limits internally.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Ensure clean GUC state from any prior test runs
ALTER SYSTEM RESET pg_textsearch.memory_limit;
ALTER SYSTEM RESET pg_textsearch.bulk_load_threshold;
ALTER SYSTEM RESET pg_textsearch.memtable_spill_threshold;
SELECT pg_reload_conf();
-- pg_reload_conf() sends SIGHUP but the backend processes it
-- lazily at the next CHECK_FOR_INTERRUPTS. A short sleep ensures
-- the new values are visible to subsequent statements.
SELECT pg_sleep(0.1);

-- Test 1: GUC default
SHOW pg_textsearch.memory_limit;

-- Test 2: SET should fail (PGC_SIGHUP context)
SET pg_textsearch.memory_limit = '64MB';

-- Test 3: bm25_memory_usage() returns data with defaults
SELECT dsa_total_bytes >= 0 AS has_dsa,
       estimated_bytes >= 0 AS has_est,
       counter_bytes >= 0 AS has_counter,
       memory_limit_mb > 0 AS has_limit
FROM bm25_memory_usage();

-- Test 4: Create table and index for subsequent tests
CREATE TABLE mem_test (id serial, body text);
CREATE INDEX idx_mem ON mem_test
    USING bm25(body) WITH (text_config='english');

-- Test 5: Per-index soft limit (limit/8) triggers spill
-- With limit=4MB, per-index soft = 512kB.
-- Disable legacy thresholds so only the new limit drives spilling.
ALTER SYSTEM SET pg_textsearch.memory_limit = '4MB';
ALTER SYSTEM SET pg_textsearch.bulk_load_threshold = 0;
ALTER SYSTEM SET pg_textsearch.memtable_spill_threshold = 0;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

SET client_min_messages = error;

-- Insert enough data to exceed 512kB estimated.
-- Each row has ~3 terms: "perlimit_N", "word_M", and a long filler.
INSERT INTO mem_test (body)
SELECT 'perlimit_' || i || ' word_' || (i+1) || ' '
       || repeat('filler_', 10)
FROM generate_series(1, 2000) i;

RESET client_min_messages;

-- Verify data was inserted (spill does not reject writes)
SELECT count(*) >= 2000 AS inserts_ok FROM mem_test;

-- Verify we can query the index (data was spilled to disk)
SELECT count(*) > 0 AS can_query FROM (
    SELECT 1 FROM mem_test
    ORDER BY body <@> 'perlimit'::bm25query
) sub;

-- Test 6: Disabled limit allows unlimited inserts
ALTER SYSTEM SET pg_textsearch.memory_limit = 0;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

-- Verify bm25_memory_usage() with disabled limit shows NULLs
SELECT memory_limit_mb IS NULL AS limit_null,
       usage_pct IS NULL AS pct_null
FROM bm25_memory_usage();

SELECT bm25_spill_index('idx_mem') IS NOT NULL AS spilled;

INSERT INTO mem_test (body)
SELECT 'nolimit_' || i || '_' || j
FROM generate_series(1, 200) i,
     generate_series(1, 5) j;

SELECT count(*) > 2000 AS unlimited_ok FROM mem_test;

-- Test 7: Hard limit blocks inserts when DSA exceeds limit
-- First, load data so DSA is large, then set a tiny limit.
ALTER SYSTEM SET pg_textsearch.memory_limit = '100kB';
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

DO $$
BEGIN
    INSERT INTO mem_test (body) VALUES ('hard_limit_test');
    RAISE NOTICE 'ERROR: expected insert to fail';
EXCEPTION
    WHEN program_limit_exceeded THEN
        RAISE NOTICE 'insert correctly blocked by memory limit';
END;
$$;

-- Test 8: Inserts succeed when within budget
ALTER SYSTEM SET pg_textsearch.memory_limit = '2GB';
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

INSERT INTO mem_test (body) VALUES ('within_limit');
SELECT count(*) > 2000 AS hard_ok FROM mem_test;

-- Test 9: Atomic counter tracks inserts, spills, and drops
-- Verify counter_bytes matches estimated_bytes (registry scan).
CREATE TABLE mem_counter (id serial, body text);
CREATE INDEX idx_counter ON mem_counter
    USING bm25(body) WITH (text_config='english');

INSERT INTO mem_counter (body)
SELECT 'counter_' || i FROM generate_series(1, 500) i;

SELECT counter_bytes > 0 AS counter_grows,
       counter_bytes = estimated_bytes AS counter_matches
FROM bm25_memory_usage();

SELECT bm25_spill_index('idx_counter') IS NOT NULL AS spilled;

SELECT counter_bytes = 0 AS counter_zeroed,
       counter_bytes = estimated_bytes AS counter_matches
FROM bm25_memory_usage();

DROP INDEX idx_counter;

SELECT counter_bytes = estimated_bytes AS counter_matches
FROM bm25_memory_usage();

DROP TABLE mem_counter;

-- Test 10: Build pre-check eviction path
-- Load data with limits disabled, then build a new index with
-- a tight limit to trigger the eviction pre-check.
ALTER SYSTEM SET pg_textsearch.memory_limit = 0;
ALTER SYSTEM SET pg_textsearch.bulk_load_threshold = 0;
ALTER SYSTEM SET pg_textsearch.memtable_spill_threshold = 0;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

SET client_min_messages = error;
INSERT INTO mem_test (body)
SELECT 'prebuild_' || i || ' ' || repeat('omega_', 10)
FROM generate_series(1, 5000) i;
RESET client_min_messages;

-- Set a tight limit so build pre-check sees the memtable.
ALTER SYSTEM SET pg_textsearch.memory_limit = '3MB';
ALTER SYSTEM RESET pg_textsearch.bulk_load_threshold;
ALTER SYSTEM RESET pg_textsearch.memtable_spill_threshold;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

CREATE TABLE mem_build_test (id serial, body text);
INSERT INTO mem_build_test (body)
SELECT 'build_test_' || i FROM generate_series(1, 10) i;

SET client_min_messages = error;
CREATE INDEX idx_build ON mem_build_test
    USING bm25(body) WITH (text_config='english');
RESET client_min_messages;

SELECT count(*) AS build_ok FROM (
    SELECT 1 FROM mem_build_test
    ORDER BY body <@> 'build_test'::bm25query
) sub;

DROP TABLE mem_build_test CASCADE;

-- Clean up
ALTER SYSTEM RESET pg_textsearch.memory_limit;
ALTER SYSTEM RESET pg_textsearch.bulk_load_threshold;
ALTER SYSTEM RESET pg_textsearch.memtable_spill_threshold;
SELECT pg_reload_conf();

DROP TABLE IF EXISTS mem_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
