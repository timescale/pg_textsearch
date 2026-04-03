-- Test pg_textsearch memory limit GUCs:
--   memtable_memory_limit (soft limit, estimated usage)
--   max_shared_memory (hard limit, DSA reservation)

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Ensure clean GUC state from any prior test runs
ALTER SYSTEM RESET pg_textsearch.memtable_memory_limit;
ALTER SYSTEM RESET pg_textsearch.max_shared_memory;
ALTER SYSTEM RESET pg_textsearch.bulk_load_threshold;
ALTER SYSTEM RESET pg_textsearch.memtable_spill_threshold;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

-- Test 1: GUC defaults
SHOW pg_textsearch.memtable_memory_limit;
SHOW pg_textsearch.max_shared_memory;

-- Test 2: SET should fail (PGC_SIGHUP context)
SET pg_textsearch.memtable_memory_limit = '64MB';
SET pg_textsearch.max_shared_memory = '64MB';

-- Test 3: Create table and index for subsequent tests
CREATE TABLE mem_test (id serial, body text);
CREATE INDEX idx_mem ON mem_test
    USING bm25(body) WITH (text_config='english');

-- Test 4: Soft limit triggers spill
-- Set a very low soft limit so inserting data will trigger eviction.
ALTER SYSTEM SET pg_textsearch.memtable_memory_limit = '100kB';
ALTER SYSTEM SET pg_textsearch.max_shared_memory = 0;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SHOW pg_textsearch.memtable_memory_limit;

-- Suppress spill WARNINGs (they contain non-deterministic values)
SET client_min_messages = error;

INSERT INTO mem_test (body)
SELECT 'softlimit_' || i || ' word_' || (i+1) || ' word_' || (i+2)
FROM generate_series(1, 500) i;

RESET client_min_messages;

-- Verify data was inserted (spill does not reject writes)
SELECT count(*) >= 500 AS inserts_ok FROM mem_test;

-- Verify we can query the index (data was spilled to segments)
SELECT count(*) > 0 AS can_query
FROM mem_test WHERE body <@> 'softlimit'::bm25query < 0;

-- Test 5: Disabled soft limit allows unlimited inserts
ALTER SYSTEM SET pg_textsearch.memtable_memory_limit = 0;
ALTER SYSTEM SET pg_textsearch.bulk_load_threshold = 0;
ALTER SYSTEM SET pg_textsearch.memtable_spill_threshold = 0;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

SELECT bm25_spill_index('idx_mem') IS NOT NULL AS spilled;

INSERT INTO mem_test (body)
SELECT 'nolimit_' || i || '_' || j
FROM generate_series(1, 200) i,
     generate_series(1, 5) j;

SELECT count(*) > 1000 AS unlimited_ok FROM mem_test;

-- Test 6: Hard limit (max_shared_memory) blocks inserts
-- Set a very low hard limit. Since the memtable already has data
-- in DSA from the unlimited inserts above, the next insert should
-- fail immediately.
ALTER SYSTEM SET pg_textsearch.max_shared_memory = '100kB';
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

-- This should ERROR because DSA usage exceeds 100kB.
-- Use DO block to check error code (exact kB values are
-- platform-dependent).
DO $$
BEGIN
    INSERT INTO mem_test (body) VALUES ('hard_limit_test');
    RAISE NOTICE 'ERROR: expected insert to fail';
EXCEPTION
    WHEN program_limit_exceeded THEN
        RAISE NOTICE 'insert correctly blocked by max_shared_memory';
END;
$$;

-- Test 7: Hard limit allows inserts when within budget
ALTER SYSTEM SET pg_textsearch.max_shared_memory = '2GB';
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

INSERT INTO mem_test (body) VALUES ('within_hard_limit');
SELECT count(*) > 1000 AS hard_ok FROM mem_test;

-- Test 8: Build with soft limit eviction
-- Reset to a moderate soft limit. Building a new index should
-- trigger eviction of existing memtables to make room.
ALTER SYSTEM SET pg_textsearch.memtable_memory_limit = '100kB';
ALTER SYSTEM SET pg_textsearch.max_shared_memory = 0;
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
ALTER SYSTEM RESET pg_textsearch.memtable_memory_limit;
ALTER SYSTEM RESET pg_textsearch.max_shared_memory;
ALTER SYSTEM RESET pg_textsearch.bulk_load_threshold;
ALTER SYSTEM RESET pg_textsearch.memtable_spill_threshold;
SELECT pg_reload_conf();

DROP TABLE IF EXISTS mem_test CASCADE;
DROP TABLE IF EXISTS mem_build_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
