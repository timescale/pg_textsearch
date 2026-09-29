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
CREATE TABLE reclaim_control (id int, body text)
    WITH (autovacuum_enabled = false);
INSERT INTO reclaim_docs
SELECT g, 'alpha beta gamma delta term' || (g % 50)
FROM generate_series(1, 2000) g;
INSERT INTO reclaim_control
SELECT g, 'alpha beta gamma delta term' || (g % 50)
FROM generate_series(1, 2000) g;

CREATE INDEX reclaim_idx ON reclaim_docs
    USING bm25 (body) WITH (text_config = 'english');
CREATE INDEX reclaim_control_idx ON reclaim_control
    USING bm25 (body) WITH (text_config = 'english');

-- Build wrote a segment directly, so the memtable is empty here; the
-- next batch fills it so the following spill produces a second segment.
INSERT INTO reclaim_docs
SELECT g, 'alpha beta term' || (g % 50)
FROM generate_series(2001, 4000) g;
INSERT INTO reclaim_control
SELECT g, 'alpha beta term' || (g % 50)
FROM generate_series(2001, 4000) g;
SELECT bm25_spill_index('reclaim_idx') > 0 AS spilled;
\pset format unaligned
SELECT bm25_spill_index('reclaim_control_idx') > 0 AS control_spilled;

-- Merge the L0 segments into one L1 segment; this displaces the source
-- segments' pages, which must be parked (not freed).
SELECT bm25_force_merge('reclaim_idx');
SELECT bm25_force_merge('reclaim_control_idx');

-- After a merge, displaced pages are parked (> 0), NOT freed.
SELECT bm25_pending_free_pages('reclaim_idx') > 0 AS parked_after_merge;

-- The control index used identical construction, so its first detached
-- batch must have the same size. Drain only that batch before performing
-- identical second merges on both indexes; its later parked count then
-- measures the second batch alone.
SELECT bm25_pending_free_pages('reclaim_idx')
    AS first_batch_count \gset
SELECT bm25_pending_free_pages('reclaim_control_idx')
    = :first_batch_count AS control_first_batch_matches;
SELECT txid_current() IS NOT NULL AS control_t1;
SELECT txid_current() IS NOT NULL AS control_t2;
VACUUM reclaim_control;
SELECT bm25_pending_free_pages('reclaim_control_idx')
    AS control_after_first_drain;

-- Attach another detached batch while the first one is still parked in
-- reclaim_idx. If publication replaced the old head, its count would equal
-- the control's second batch alone instead of first_batch + control_batch.
INSERT INTO reclaim_docs
SELECT g, 'alpha beta second merge term' || (g % 50)
FROM generate_series(4001, 5000) g;
INSERT INTO reclaim_control
SELECT g, 'alpha beta second merge term' || (g % 50)
FROM generate_series(4001, 5000) g;
SELECT bm25_spill_index('reclaim_idx') > 0 AS second_spilled;
SELECT bm25_spill_index('reclaim_control_idx') > 0
    AS control_second_spilled;
SELECT bm25_force_merge('reclaim_idx');
SELECT bm25_force_merge('reclaim_control_idx');
SELECT bm25_pending_free_pages('reclaim_idx')
    = :first_batch_count
      + bm25_pending_free_pages('reclaim_control_idx')
    AS preserved_old_tombstones;
\pset format aligned
DROP TABLE reclaim_control;

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
FROM generate_series(5001, 6000) g;
SELECT bm25_spill_index('reclaim_idx') > 0 AS reuse_spilled;
SELECT pg_relation_size('reclaim_idx') / current_setting('block_size')::int
    <= :blocks_before_reuse AS reused_freed_pages_no_extension;

-- VACUUM's own segment replacement/drop path must also park displaced
-- pages instead of returning them to the FSM immediately (#413). Build a
-- fresh index layout so the observation is independent of the merge case
-- above.
DROP INDEX reclaim_idx;
TRUNCATE reclaim_docs;
INSERT INTO reclaim_docs
SELECT g, 'vacuum alpha beta term' || (g % 50)
FROM generate_series(1, 1500) g;
CREATE INDEX reclaim_idx ON reclaim_docs
    USING bm25 (body) WITH (text_config = 'english');

-- Add a second segment through the on-disk memtable, then delete exactly
-- those rows so VACUUM drops that all-dead segment.  First create an FSM
-- free-page pool: unlocked VACUUM preparation may safely allocate its
-- detached tombstone pages there instead of extending the relation.
INSERT INTO reclaim_docs
SELECT g, 'vacuum pool beta term' || (g % 50)
FROM generate_series(1501, 3000) g;
\pset format unaligned
SELECT bm25_spill_index('reclaim_idx') > 0 AS vacuum_pool_spilled;
SELECT bm25_force_merge('reclaim_idx');
SELECT txid_current() IS NOT NULL AS vp1;
SELECT txid_current() IS NOT NULL AS vp2;
VACUUM reclaim_docs;
SELECT bm25_pending_free_pages('reclaim_idx') AS pool_after_vacuum;

INSERT INTO reclaim_docs
SELECT g, 'vacuum dropped beta term' || (g % 50)
FROM generate_series(3001, 3020) g;
SELECT bm25_spill_index('reclaim_idx') > 0 AS vacuum_spilled;
SELECT pg_relation_size('reclaim_idx') / current_setting('block_size')::int
    AS blocks_before_vacuum_drop \gset
DELETE FROM reclaim_docs WHERE id BETWEEN 3001 AND 3020;
VACUUM reclaim_docs;
SELECT pg_relation_size('reclaim_idx') / current_setting('block_size')::int
    <= :blocks_before_vacuum_drop AS vacuum_tombstone_reused_fsm;

-- VACUUM may retain the pages or reclaim them immediately if concurrent
-- transactions already advanced the horizon. A later VACUUM drains any
-- retained pages.  segment_reclaim_injection.sql pins the horizon with
-- an injection point to assert the parking behaviour deterministically.
SELECT txid_current() IS NOT NULL AS vt1;
SELECT txid_current() IS NOT NULL AS vt2;
VACUUM reclaim_docs;
SELECT bm25_pending_free_pages('reclaim_idx') AS parked_after_vacuum_drain;

-- The drained pages must be reusable from the FSM without extending the
-- index relation.
SELECT pg_relation_size('reclaim_idx') / current_setting('block_size')::int
    AS blocks_before_vacuum_reuse \gset
INSERT INTO reclaim_docs
SELECT g, 'vacuum reuse beta term' || (g % 50)
FROM generate_series(3021, 3720) g;
SELECT bm25_spill_index('reclaim_idx') > 0 AS vacuum_reuse_spilled;
SELECT pg_relation_size('reclaim_idx') / current_setting('block_size')::int
    <= :blocks_before_vacuum_reuse AS vacuum_reused_freed_pages_no_extension;
\pset format aligned

-- Force merge may truncate only pages that reclaim has already stamped
-- recyclable. Build a free-page pool below a one-page memtable at EOF, then
-- let force merge spill into that pool. The retired memtable page remains
-- protected by dead_fxid and must survive until VACUUM emits conflict WAL and
-- stamps it TP_FREE_PAGE_MAGIC.
\pset format unaligned
CREATE TABLE truncate_docs (id int, body text)
    WITH (autovacuum_enabled = false);
INSERT INTO truncate_docs
SELECT g, 'truncate alpha beta gamma ' || g || ' ' ||
       repeat(md5(g::text), 4)
FROM generate_series(1, 2000) g;
CREATE INDEX truncate_idx ON truncate_docs
    USING bm25 (body)
    WITH (text_config = 'english', compaction = 'off');
INSERT INTO truncate_docs
SELECT g, 'truncate alpha beta delta ' || g || ' ' ||
       repeat(md5(g::text), 4)
FROM generate_series(2001, 4000) g;
SELECT bm25_spill_index('truncate_idx') > 0 AS truncate_pool_spilled;
SELECT bm25_force_merge('truncate_idx');
SELECT bm25_pending_free_pages('truncate_idx') > 0
    AS truncate_pool_parked;

INSERT INTO truncate_docs VALUES (4001, 'truncate eof marker');
CREATE TEMP TABLE truncate_suffix AS
SELECT max(blkno)::bigint AS blkno
FROM bm25_memtable_chain('truncate_idx');
SELECT blkno + 1 =
           pg_relation_size('truncate_idx'::regclass, 'main') /
           current_setting('block_size')::int
    AS truncate_memtable_at_eof
FROM truncate_suffix;

SELECT txid_current() IS NOT NULL AS truncate_t1;
SELECT txid_current() IS NOT NULL AS truncate_t2;
VACUUM truncate_docs;
SELECT bm25_pending_free_pages('truncate_idx') = 0
    AS truncate_pool_drained;

SELECT bm25_force_merge('truncate_idx');
SELECT EXISTS (
           SELECT 1
           FROM bm25_memtable_dead_pages('truncate_idx') dead,
                truncate_suffix suffix
           WHERE dead.blkno = suffix.blkno
             AND dead.dead_fxid > 0
       )
       AND (
           SELECT blkno <
                  pg_relation_size('truncate_idx'::regclass, 'main') /
                  current_setting('block_size')::int
           FROM truncate_suffix
       )
    AS force_merge_preserved_unreclaimed_suffix;

SELECT txid_current() IS NOT NULL AS truncate_reclaim_t1;
SELECT txid_current() IS NOT NULL AS truncate_reclaim_t2;
VACUUM truncate_docs;
CREATE TEMP TABLE truncate_recyclable_size AS
SELECT pg_relation_size('truncate_idx'::regclass, 'main') /
       current_setting('block_size')::int AS blocks;
SELECT bm25_force_merge('truncate_idx');
SELECT before.blocks >
           pg_relation_size('truncate_idx'::regclass, 'main') /
           current_setting('block_size')::int
       AND suffix.blkno >=
           pg_relation_size('truncate_idx'::regclass, 'main') /
           current_setting('block_size')::int
    AS force_merge_truncated_recyclable_suffix
FROM truncate_recyclable_size before,
     truncate_suffix suffix;

DROP TABLE truncate_docs;
\pset format aligned
DROP TABLE reclaim_docs;
DROP EXTENSION pg_textsearch CASCADE;
