-- Tests the on-disk memtable chain lifecycle introduced by the
-- page-backed memtable rewrite (issue #374):
--
--   * inserts append to chain pages,
--   * manual bm25_spill_index() drains the chain into an L0
--     segment,
--   * memtable_pages_threshold triggers an auto-spill at
--     transaction end,
--   * queries return correct results across spills.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

CREATE TABLE memtable_spill_test (id serial PRIMARY KEY, body text);
CREATE INDEX memtable_spill_idx ON memtable_spill_test
    USING bm25(body) WITH (text_config = 'english');

-- Test 1: a freshly built empty index has no chain pages
--         (metapage.memtable_head_blkno = InvalidBlockNumber).
SELECT count(*) AS chain_pages_at_create
FROM bm25_memtable_chain('memtable_spill_idx');

-- Test 2: inserts populate the chain.  Use enough distinct rows
-- that more than one chain page is required (records are
-- ~40-80 bytes after packing, plus per-doc TpVector overhead).
INSERT INTO memtable_spill_test (body)
SELECT 'alpha doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 200) i;

SELECT count(*) > 1 AS chain_grew_past_one_page
FROM bm25_memtable_chain('memtable_spill_idx');

-- Test 3: queries see memtable contents directly (no segments yet).
SELECT count(*) AS alpha_hits_before_spill FROM (
    SELECT 1 FROM memtable_spill_test
    ORDER BY body <@> to_bm25query('alpha', 'memtable_spill_idx')
    LIMIT 1000
) sub;

-- Test 4: manual spill flushes the chain into an L0 segment and
-- resets metapage.memtable_head_blkno/tail_blkno to Invalid.
SELECT bm25_spill_index('memtable_spill_idx') IS NOT NULL
       AS produced_segment;

SELECT count(*) AS chain_pages_after_spill
FROM bm25_memtable_chain('memtable_spill_idx');

-- Test 5: queries still return the same answers (data is now in
-- the segment, not the memtable).
SELECT count(*) AS alpha_hits_after_spill FROM (
    SELECT 1 FROM memtable_spill_test
    ORDER BY body <@> to_bm25query('alpha', 'memtable_spill_idx')
    LIMIT 1000
) sub;

-- Test 6: spilling an empty chain is a no-op and returns NULL.
SELECT bm25_spill_index('memtable_spill_idx') AS empty_chain_spill;

-- Test 7: subsequent inserts build a fresh chain.  Run them in
-- a transaction with the auto-spill threshold disabled so the
-- chain remains observable at COMMIT.
SET pg_textsearch.memtable_pages_threshold = 0;
BEGIN;
INSERT INTO memtable_spill_test (body)
SELECT 'beta ' || i FROM generate_series(1, 30) i;

SELECT count(*) > 0 AS chain_rebuilt
FROM bm25_memtable_chain('memtable_spill_idx');
COMMIT;

-- After commit with threshold = 0 the chain still has the new
-- entries; verify they survive across the COMMIT boundary.
SELECT count(*) > 0 AS chain_persists_after_commit
FROM bm25_memtable_chain('memtable_spill_idx');

-- Test 8: memtable_pages_threshold (in chain pages) drains the
-- chain at COMMIT.  Spill once to start from empty, then insert
-- past the threshold and verify the chain ends empty.
SELECT bm25_spill_index('memtable_spill_idx') IS NOT NULL OR true
       AS reset;

SET pg_textsearch.memtable_pages_threshold = 1;
INSERT INTO memtable_spill_test (body)
SELECT 'gamma ' || i || ' delta ' || (i+1) || ' epsilon ' || (i+2)
FROM generate_series(1, 100) i;

SELECT count(*) AS chain_pages_after_autospill
FROM bm25_memtable_chain('memtable_spill_idx');

RESET pg_textsearch.memtable_pages_threshold;

-- Test 9: query the (now-spilled) data to confirm end-to-end
-- correctness across the auto-spill path.
SELECT count(*) > 0 AS gamma_queryable FROM (
    SELECT 1 FROM memtable_spill_test
    ORDER BY body <@> to_bm25query('gamma', 'memtable_spill_idx')
    LIMIT 1000
) sub;

DROP TABLE memtable_spill_test;
DROP EXTENSION pg_textsearch CASCADE;
