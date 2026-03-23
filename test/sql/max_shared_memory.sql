-- Test pg_textsearch.max_shared_memory GUC and memory limit enforcement

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Test 1: GUC default
SHOW pg_textsearch.max_shared_memory;

-- Test 2: SET should fail (PGC_SIGHUP context)
SET pg_textsearch.max_shared_memory = '64MB';

-- Test 3: bm25_memory_usage() with no limit set
SELECT total_dsa_bytes >= 0 AS has_dsa,
       max_memory_bytes IS NULL AS limit_disabled,
       max_memory_mb IS NULL AS limit_mb_null,
       usage_pct IS NULL AS usage_null
FROM bm25_memory_usage();

-- Test 4: Set max_shared_memory via ALTER SYSTEM and reload
ALTER SYSTEM SET pg_textsearch.max_shared_memory = '512MB';
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);
SHOW pg_textsearch.max_shared_memory;

-- Test 5: bm25_memory_usage() now reflects the limit
SELECT max_memory_bytes IS NOT NULL AS limit_set,
       max_memory_mb > 0 AS has_limit_mb,
       usage_pct IS NOT NULL AS has_usage_pct
FROM bm25_memory_usage();

-- Test 6: Create table and index, verify memory tracking works
CREATE TABLE max_mem_test (id serial, body text);
CREATE INDEX idx_max_mem ON max_mem_test
    USING bm25(body) WITH (text_config='english');

INSERT INTO max_mem_test (body)
SELECT 'word_' || i || ' word_' || (i+1) || ' word_' || (i+2)
FROM generate_series(1, 100) i;

SELECT total_dsa_bytes > 0 AS dsa_grew
FROM bm25_memory_usage();

-- Test 7: Verify max_shared_memory = 0 (disabled) allows unlimited inserts
ALTER SYSTEM RESET pg_textsearch.max_shared_memory;
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

SET pg_textsearch.bulk_load_threshold = 0;
SET pg_textsearch.memtable_spill_threshold = 0;

INSERT INTO max_mem_test (body)
SELECT 'unlimited_' || i || '_' || j
FROM generate_series(1, 200) i,
     generate_series(1, 5) j;

SELECT count(*) > 100 AS inserts_ok FROM max_mem_test;

-- Test 8: Low memory limit triggers spill during inserts
-- Spill first so we start from a clean memtable
SELECT bm25_spill_index('idx_max_mem') IS NOT NULL AS spilled;

ALTER SYSTEM SET pg_textsearch.max_shared_memory = '100kB';
SELECT pg_reload_conf();
SELECT pg_sleep(0.1);

-- Suppress spill WARNINGs (they contain non-deterministic OIDs)
SET client_min_messages = error;

INSERT INTO max_mem_test (body)
SELECT 'fill_' || i
FROM generate_series(1, 500) i;

-- Verify data was inserted (writes are never rejected)
SELECT count(*) > 1000 AS all_inserts_ok FROM max_mem_test;

RESET client_min_messages;

-- Test 9: Build blocked at limit
CREATE TABLE max_mem_build_test (id serial, body text);
INSERT INTO max_mem_build_test (body)
SELECT 'build_test_' || i FROM generate_series(1, 10) i;

-- This should ERROR because DSA > 100kB.
-- The exact kB values in the error vary by platform, so we
-- verify the error code rather than the exact message.
DO $$
BEGIN
    EXECUTE 'CREATE INDEX idx_max_mem_build ON max_mem_build_test '
            'USING bm25(body) WITH (text_config=''english'')';
    RAISE NOTICE 'ERROR: expected build to fail but it succeeded';
EXCEPTION
    WHEN program_limit_exceeded THEN
        RAISE NOTICE 'build correctly blocked by max_shared_memory';
END;
$$;

-- Clean up
ALTER SYSTEM RESET pg_textsearch.max_shared_memory;
SELECT pg_reload_conf();

DROP TABLE IF EXISTS max_mem_test CASCADE;
DROP TABLE IF EXISTS max_mem_build_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
