-- Deferred, standby-safe reclaim of displaced segment pages (#380).
-- A merge must PARK displaced pages in the tombstone chain rather than
-- freeing them immediately; a later VACUUM (once the global xid horizon
-- has advanced past the merge stamp) reclaims them.
--
-- autovacuum is disabled on the table so the only drain is the explicit
-- VACUUM below, keeping the parked-page observations deterministic.
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
CREATE TABLE reclaim_docs (id int, body text)
    WITH (autovacuum_enabled = false);
INSERT INTO reclaim_docs
SELECT g, 'alpha beta gamma delta term' || (g % 50)
FROM generate_series(1, 2000) g;

CREATE INDEX reclaim_idx ON reclaim_docs
    USING bm25 (body) WITH (text_config = 'english');

-- Build wrote a segment directly, so the memtable is empty here; the
-- next batch fills it so the following spill produces a second segment.
INSERT INTO reclaim_docs
SELECT g, 'alpha beta term' || (g % 50)
FROM generate_series(2001, 4000) g;
SELECT bm25_spill_index('reclaim_idx') > 0 AS spilled;

-- Merge the L0 segments into one L1 segment; this displaces the source
-- segments' pages, which must be parked (not freed).
SELECT bm25_force_merge('reclaim_idx');

-- After a merge, displaced pages are parked (> 0), NOT freed.
SELECT bm25_pending_free_pages('reclaim_idx') > 0 AS parked_after_merge;

-- Advance the global xid horizon past the merge stamp so the parked
-- pages become reclaimable, then VACUUM to drain them.
SELECT txid_current() IS NOT NULL AS t1;
SELECT txid_current() IS NOT NULL AS t2;
VACUUM reclaim_docs;

-- All parked pages reclaimed.
SELECT bm25_pending_free_pages('reclaim_idx') AS parked_after_vacuum;

-- Sanity: queries still work after reclaim.
SELECT count(*) > 0 AS has_hits
FROM (
    SELECT 1 FROM reclaim_docs
    ORDER BY body <@> to_bm25query('alpha', 'reclaim_idx')
    LIMIT 10
) s;

-- The drained pages must be genuinely reusable from the FSM, not just
-- unlinked from the tombstone chain: allocating a fresh segment must
-- pull those freed blocks via GetFreeIndexPage rather than extend the
-- index relation.  Capture the size, spill a new segment, and assert
-- the relation did not grow.  A regression that recorded the frees but
-- skipped IndexFreeSpaceMapVacuum would still empty the chain above yet
-- extend the relation here (the freed blocks would not be discoverable
-- by the root-down GetFreeIndexPage search).
SELECT pg_relation_size('reclaim_idx') / current_setting('block_size')::int
    AS blocks_before_reuse \gset
INSERT INTO reclaim_docs
SELECT g, 'alpha beta term' || (g % 50)
FROM generate_series(4001, 5000) g;
SELECT bm25_spill_index('reclaim_idx') > 0 AS reuse_spilled;
SELECT pg_relation_size('reclaim_idx') / current_setting('block_size')::int
    <= :blocks_before_reuse AS reused_freed_pages_no_extension;

DROP TABLE reclaim_docs;
DROP EXTENSION pg_textsearch CASCADE;
