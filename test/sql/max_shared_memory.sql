-- Test pg_textsearch memory limit GUCs:
--   soft_limit_one_memtable (per-index estimated size)
--   soft_limit_all_memtables (global estimated size)
--   hard_limit_all_memtables (DSA reservation)

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Ensure clean GUC state from any prior test runs
ALTER SYSTEM RESET pg_textsearch.soft_limit_one_memtable;
ALTER SYSTEM RESET pg_textsearch.soft_limit_all_memtables;
ALTER SYSTEM RESET pg_textsearch.hard_limit_all_memtables;
ALTER SYSTEM RESET pg_textsearch.bulk_load_threshold;
ALTER SYSTEM RESET pg_textsearch.memtable_spill_threshold;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

-- Test 1: GUC defaults
SHOW pg_textsearch.soft_limit_one_memtable;
SHOW pg_textsearch.soft_limit_all_memtables;
SHOW pg_textsearch.hard_limit_all_memtables;

-- Test 2: SET should fail (PGC_SIGHUP context)
SET pg_textsearch.soft_limit_one_memtable = '64MB';
SET pg_textsearch.soft_limit_all_memtables = '64MB';
SET pg_textsearch.hard_limit_all_memtables = '64MB';

-- Test 3: bm25_memory_usage() returns data with defaults
SELECT dsa_total_bytes >= 0 AS has_dsa,
       estimated_bytes >= 0 AS has_est,
       soft_limit_mb > 0 AS has_soft,
       hard_limit_mb > 0 AS has_hard
FROM bm25_memory_usage();

-- Test 4: Create table and index for subsequent tests
CREATE TABLE mem_test (id serial, body text);
CREATE INDEX idx_mem ON mem_test
    USING bm25(body) WITH (text_config='english');

-- Test 4: Per-index soft limit triggers spill
ALTER SYSTEM SET pg_textsearch.soft_limit_one_memtable = '50kB';
ALTER SYSTEM SET pg_textsearch.soft_limit_all_memtables = 0;
ALTER SYSTEM SET pg_textsearch.hard_limit_all_memtables = 0;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

SET client_min_messages = error;

INSERT INTO mem_test (body)
SELECT 'perlimit_' || i || ' word_' || (i+1)
FROM generate_series(1, 500) i;

RESET client_min_messages;

-- Verify data was inserted (spill does not reject writes)
SELECT count(*) >= 500 AS inserts_ok FROM mem_test;

-- Verify we can query the index (data was spilled)
SELECT count(*) > 0 AS can_query
FROM mem_test WHERE body <@> 'perlimit'::bm25query < 0;

-- Test 5: Global soft limit triggers cross-index eviction
ALTER SYSTEM SET pg_textsearch.soft_limit_one_memtable = 0;
ALTER SYSTEM SET pg_textsearch.soft_limit_all_memtables = '100kB';
ALTER SYSTEM SET pg_textsearch.hard_limit_all_memtables = 0;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

SET client_min_messages = error;

INSERT INTO mem_test (body)
SELECT 'globallimit_' || i || ' word_' || (i+1)
FROM generate_series(1, 500) i;

RESET client_min_messages;

SELECT count(*) >= 1000 AS global_ok FROM mem_test;

-- Verify bm25_memory_usage() shows estimated usage and soft limit
SELECT estimated_bytes > 0 AS has_est,
       soft_limit_mb IS NOT NULL AS has_soft,
       soft_usage_pct IS NOT NULL AS has_pct
FROM bm25_memory_usage();

-- Test 6: Disabled limits allow unlimited inserts
ALTER SYSTEM SET pg_textsearch.soft_limit_one_memtable = 0;
ALTER SYSTEM SET pg_textsearch.soft_limit_all_memtables = 0;
ALTER SYSTEM SET pg_textsearch.hard_limit_all_memtables = 0;
ALTER SYSTEM SET pg_textsearch.bulk_load_threshold = 0;
ALTER SYSTEM SET pg_textsearch.memtable_spill_threshold = 0;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

-- Verify bm25_memory_usage() with disabled limits shows NULLs
SELECT soft_limit_mb IS NULL AS soft_null,
       hard_limit_mb IS NULL AS hard_null,
       soft_usage_pct IS NULL AS pct_null
FROM bm25_memory_usage();

SELECT bm25_spill_index('idx_mem') IS NOT NULL AS spilled;

INSERT INTO mem_test (body)
SELECT 'nolimit_' || i || '_' || j
FROM generate_series(1, 200) i,
     generate_series(1, 5) j;

SELECT count(*) > 1000 AS unlimited_ok FROM mem_test;

-- Test 7: Hard limit blocks inserts
ALTER SYSTEM SET pg_textsearch.hard_limit_all_memtables = '100kB';
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

DO $$
BEGIN
    INSERT INTO mem_test (body) VALUES ('hard_limit_test');
    RAISE NOTICE 'ERROR: expected insert to fail';
EXCEPTION
    WHEN program_limit_exceeded THEN
        RAISE NOTICE 'insert correctly blocked by hard limit';
END;
$$;

-- Test 8: Hard limit allows inserts when within budget
ALTER SYSTEM SET pg_textsearch.hard_limit_all_memtables = '2GB';
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

INSERT INTO mem_test (body) VALUES ('within_hard_limit');
SELECT count(*) > 1000 AS hard_ok FROM mem_test;

-- Test 9: Build with soft limit eviction
ALTER SYSTEM SET pg_textsearch.soft_limit_all_memtables = '100kB';
ALTER SYSTEM SET pg_textsearch.hard_limit_all_memtables = '2GB';
ALTER SYSTEM RESET pg_textsearch.bulk_load_threshold;
ALTER SYSTEM RESET pg_textsearch.memtable_spill_threshold;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

CREATE TABLE mem_build_test (id serial, body text);
INSERT INTO mem_build_test (body)
SELECT 'build_test_' || i FROM generate_series(1, 10) i;

SET client_min_messages = error;
CREATE INDEX idx_mem_build ON mem_build_test
    USING bm25(body) WITH (text_config='english');
RESET client_min_messages;

SELECT count(*) AS build_ok FROM mem_build_test
    WHERE body <@> 'build_test'::bm25query < 0;

-- Clean up
ALTER SYSTEM RESET pg_textsearch.soft_limit_one_memtable;
ALTER SYSTEM RESET pg_textsearch.soft_limit_all_memtables;
ALTER SYSTEM RESET pg_textsearch.hard_limit_all_memtables;
ALTER SYSTEM RESET pg_textsearch.bulk_load_threshold;
ALTER SYSTEM RESET pg_textsearch.memtable_spill_threshold;
SELECT pg_reload_conf();

DROP TABLE IF EXISTS mem_test CASCADE;
DROP TABLE IF EXISTS mem_build_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
