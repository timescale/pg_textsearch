-- Integration coverage for the spill -> cache-invalidate -> rebuild
-- cycle (cache_source.c + log.c; see docs/memtable_cache.md).
--
-- tp_spill_finalize (a) bumps the per-index spill_generation atomic
-- and (b) drops the cache's dshash tables via tp_cache_clear, so a
-- query that runs after a spill rebuilds a fresh cache from
-- whatever chain pages exist post-spill (zero in the typical case,
-- since spill resets the chain head/tail to InvalidBlockNumber).
--
-- We exercise both with the cache GUC explicitly enabled so the
-- chooser routes reads through cache_source.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET pg_textsearch.memtable_cache_enabled = on;

CREATE TABLE cache_spill_t (id int, body text);
CREATE INDEX cache_spill_idx ON cache_spill_t
    USING bm25 (body) WITH (text_config = 'english');

-- ---------- populate + open cache, pre-spill ----------
INSERT INTO cache_spill_t
SELECT g, 'alpha term ' || g
  FROM generate_series(1, 10) g;

-- Cache source opens against the populated memtable.  total_docs
-- should be 10; we check structurally.
SELECT split_part(bm25_test_cache_source('cache_spill_idx', 'open'),
                  ',', 1) AS open_docs;

-- The term 'alpha' is in every doc so doc_freq = 10.
SELECT bm25_test_cache_source('cache_spill_idx', 'lookup:alpha');

-- End-to-end query through the chooser returns all 10 rows.
SELECT count(*) AS pre_spill_hits FROM (
    SELECT id FROM cache_spill_t
    ORDER BY body <@> to_bm25query('alpha', 'cache_spill_idx')
    LIMIT 100
) q;

-- ---------- force a spill ----------
-- bm25_spill_index drives tp_do_spill -> tp_spill_finalize, which
-- bumps spill_generation and clears the cache.
SELECT bm25_spill_index('cache_spill_idx') > 0 AS spill_wrote_docs;

-- After spill the chain is empty (head/tail reset).  The cache
-- source constructor returns NULL on empty memtable.
SELECT bm25_test_cache_source('cache_spill_idx', 'empty');

-- The query still returns 10 rows: cache_source returns NULL,
-- chain source returns NULL, the scoring path falls through to
-- segments only and finds all 10 docs in the new segment.
SELECT count(*) AS post_spill_hits FROM (
    SELECT id FROM cache_spill_t
    ORDER BY body <@> to_bm25query('alpha', 'cache_spill_idx')
    LIMIT 100
) q;

-- ---------- populate again post-spill, cache rebuilds cold ----------
INSERT INTO cache_spill_t
SELECT g, 'beta term ' || g
  FROM generate_series(11, 15) g;

-- New cache source opens over the 5 fresh chain records (the
-- alpha docs are in the segment now, not the chain).
SELECT split_part(bm25_test_cache_source('cache_spill_idx', 'open'),
                  ',', 1) AS post_spill_open_docs;

-- 'beta' is unique to the new chain records.
SELECT bm25_test_cache_source('cache_spill_idx', 'lookup:beta');

-- 'alpha' is in the segment but not the new chain; cache lookup
-- misses for it.
SELECT bm25_test_cache_source('cache_spill_idx', 'lookup:alpha');

-- End-to-end queries see both alpha (10, segment) and beta (5,
-- cache) with no duplicates.
SELECT count(*) AS alpha_hits FROM (
    SELECT id FROM cache_spill_t
    ORDER BY body <@> to_bm25query('alpha', 'cache_spill_idx')
    LIMIT 100
) q;

SELECT count(*) AS beta_hits FROM (
    SELECT id FROM cache_spill_t
    ORDER BY body <@> to_bm25query('beta', 'cache_spill_idx')
    LIMIT 100
) q;

-- ---------- equivalence with cache disabled ----------
-- Same queries with the chooser falling back to the chain must
-- return identical counts.
SET pg_textsearch.memtable_cache_enabled = off;

SELECT count(*) AS alpha_hits_chain FROM (
    SELECT id FROM cache_spill_t
    ORDER BY body <@> to_bm25query('alpha', 'cache_spill_idx')
    LIMIT 100
) q;

SELECT count(*) AS beta_hits_chain FROM (
    SELECT id FROM cache_spill_t
    ORDER BY body <@> to_bm25query('beta', 'cache_spill_idx')
    LIMIT 100
) q;

RESET pg_textsearch.memtable_cache_enabled;

DROP INDEX cache_spill_idx;
DROP TABLE cache_spill_t;
DROP EXTENSION pg_textsearch CASCADE;
