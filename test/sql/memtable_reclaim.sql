-- Regression: Step 2 FSM reclaim of DEAD memtable orphans.
-- After spill + VACUUM, tp_reclaim_dead_memtable_pages returns blocks
-- to the index FSM; tp_memtable_alloc_page should reuse them when the
-- chain grows again (main fork should not grow by ~one block per dead
-- page if reuse works).

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- True when another same-database client backend holds an xmin, which
-- pins the reclaim horizon back and can defer FSM reclaim.
CREATE FUNCTION other_backend_holds_xmin() RETURNS boolean
LANGUAGE sql STABLE AS $$
    SELECT EXISTS (
        SELECT 1
        FROM pg_stat_activity
        WHERE pid <> pg_backend_pid()
          AND datname = current_database()
          AND backend_type = 'client backend'
          AND backend_xmin IS NOT NULL
    );
$$;

CREATE TABLE memtable_reclaim_t (id serial PRIMARY KEY, body text);
CREATE INDEX memtable_reclaim_idx ON memtable_reclaim_t
    USING bm25(body) WITH (text_config = 'english');

SELECT count(*)::int AS dead_pages_before_spill
FROM bm25_memtable_dead_pages('memtable_reclaim_idx');

INSERT INTO memtable_reclaim_t (body)
SELECT 'reclaim ' || i || ' ' || repeat('pad ', 6)
FROM generate_series(1, 200) i;

CREATE TEMP TABLE spill_stats AS
SELECT count(*)::int AS live_pages
FROM bm25_memtable_chain('memtable_reclaim_idx');

SELECT live_pages > 1 AS chain_multi_page FROM spill_stats;

SELECT bm25_spill_index('memtable_reclaim_idx') IS NOT NULL AS spill_ok;

SELECT count(*)::int AS live_chain_after_spill
FROM bm25_memtable_chain('memtable_reclaim_idx');

CREATE TEMP TABLE reclaim_dead AS
SELECT count(*)::int AS dead_count
FROM bm25_memtable_dead_pages('memtable_reclaim_idx');

SELECT dead_count > 0 AS spilled_dead_pages FROM reclaim_dead;

SELECT
    r.dead_count = s.live_pages AS dead_matches_spilled_chain
FROM reclaim_dead r,
     spill_stats s;

CREATE TEMP TABLE reclaim_sizes AS
SELECT pg_relation_size('memtable_reclaim_idx'::regclass, 'main')::bigint
       AS sz_after_spill;

CREATE TEMP TABLE reclaim_blocker_state (
    blocked_by_other_backend bool
);

INSERT INTO reclaim_blocker_state (blocked_by_other_backend)
SELECT false;

-- Horizon: spill xact is committed (autocommit per statement).
UPDATE reclaim_blocker_state
SET blocked_by_other_backend = blocked_by_other_backend
    OR other_backend_holds_xmin();
VACUUM ANALYZE memtable_reclaim_t;

-- Idempotent second pass (RecordFreeIndexPage on same blocks is safe).
UPDATE reclaim_blocker_state
SET blocked_by_other_backend = blocked_by_other_backend
    OR other_backend_holds_xmin();
VACUUM ANALYZE memtable_reclaim_t;

SELECT count(*) >= 1 AS search_after_vacuum FROM (
    SELECT 1 FROM memtable_reclaim_t
    ORDER BY body <@> to_bm25query('reclaim', 'memtable_reclaim_idx')
    LIMIT 10
) sub;

-- Rebuild chain without auto-spill so pages come from FSM vs extend.
SET pg_textsearch.memtable_pages_threshold = 0;

INSERT INTO memtable_reclaim_t (body)
SELECT 'reclaim2 ' || i || ' ' || repeat('pad ', 6)
FROM generate_series(1, 200) i;

SELECT count(*)::int > 0 AS chain_rebuilt
FROM bm25_memtable_chain('memtable_reclaim_idx');

-- Unblocked: VACUUM freed the DEAD pages and this rebuild reused them, so
-- the main fork does not grow and no DEAD pages remain.  Blocked: reclaim
-- is legitimately deferred, so skip the check rather than relax it.
SELECT
    CASE
        WHEN (SELECT blocked_by_other_backend FROM reclaim_blocker_state)
            THEN true
        ELSE (pg_relation_size('memtable_reclaim_idx'::regclass, 'main')
              - (SELECT sz_after_spill FROM reclaim_sizes)) = 0
             AND (SELECT count(*)::int
                  FROM bm25_memtable_dead_pages('memtable_reclaim_idx')) = 0
    END
    AS single_cycle_growth_valid;

-- Multi-cycle: five spill→VACUUM cycles (VACUUM cannot run inside DO).
-- With per-cycle FSM reuse, cumulative growth stays bounded by the live
-- memtable work per cycle; without reuse it would grow ~N×K pages.
CREATE TEMP TABLE multi_cycle_bounds (
    sz_start bigint,
    max_live_pages int,
    num_cycles int
);

INSERT INTO multi_cycle_bounds (sz_start, max_live_pages, num_cycles)
SELECT pg_relation_size('memtable_reclaim_idx'::regclass, 'main')::bigint,
       0,
       5;

CREATE TEMP TABLE multi_cycle_blocker_state (
    blocked_by_other_backend bool
);

INSERT INTO multi_cycle_blocker_state (blocked_by_other_backend)
SELECT false;

-- Cycle 1
INSERT INTO memtable_reclaim_t (body)
SELECT 'mc1 doc ' || g || ' ' || repeat('pad ', 6)
FROM generate_series(1, 200) g;
UPDATE multi_cycle_bounds SET max_live_pages = GREATEST(
    max_live_pages,
    (SELECT count(*)::int FROM bm25_memtable_chain('memtable_reclaim_idx')));
SELECT bm25_spill_index('memtable_reclaim_idx') IS NOT NULL AS mc_spill_1;
UPDATE multi_cycle_blocker_state
SET blocked_by_other_backend = blocked_by_other_backend
    OR other_backend_holds_xmin();
VACUUM ANALYZE memtable_reclaim_t;

-- Cycle 2
INSERT INTO memtable_reclaim_t (body)
SELECT 'mc2 doc ' || g || ' ' || repeat('pad ', 6)
FROM generate_series(1, 200) g;
UPDATE multi_cycle_bounds SET max_live_pages = GREATEST(
    max_live_pages,
    (SELECT count(*)::int FROM bm25_memtable_chain('memtable_reclaim_idx')));
SELECT bm25_spill_index('memtable_reclaim_idx') IS NOT NULL AS mc_spill_2;
UPDATE multi_cycle_blocker_state
SET blocked_by_other_backend = blocked_by_other_backend
    OR other_backend_holds_xmin();
VACUUM ANALYZE memtable_reclaim_t;

-- Cycle 3
INSERT INTO memtable_reclaim_t (body)
SELECT 'mc3 doc ' || g || ' ' || repeat('pad ', 6)
FROM generate_series(1, 200) g;
UPDATE multi_cycle_bounds SET max_live_pages = GREATEST(
    max_live_pages,
    (SELECT count(*)::int FROM bm25_memtable_chain('memtable_reclaim_idx')));
SELECT bm25_spill_index('memtable_reclaim_idx') IS NOT NULL AS mc_spill_3;
UPDATE multi_cycle_blocker_state
SET blocked_by_other_backend = blocked_by_other_backend
    OR other_backend_holds_xmin();
VACUUM ANALYZE memtable_reclaim_t;

-- Cycle 4
INSERT INTO memtable_reclaim_t (body)
SELECT 'mc4 doc ' || g || ' ' || repeat('pad ', 6)
FROM generate_series(1, 200) g;
UPDATE multi_cycle_bounds SET max_live_pages = GREATEST(
    max_live_pages,
    (SELECT count(*)::int FROM bm25_memtable_chain('memtable_reclaim_idx')));
SELECT bm25_spill_index('memtable_reclaim_idx') IS NOT NULL AS mc_spill_4;
UPDATE multi_cycle_blocker_state
SET blocked_by_other_backend = blocked_by_other_backend
    OR other_backend_holds_xmin();
VACUUM ANALYZE memtable_reclaim_t;

-- Cycle 5
INSERT INTO memtable_reclaim_t (body)
SELECT 'mc5 doc ' || g || ' ' || repeat('pad ', 6)
FROM generate_series(1, 200) g;
UPDATE multi_cycle_bounds SET max_live_pages = GREATEST(
    max_live_pages,
    (SELECT count(*)::int FROM bm25_memtable_chain('memtable_reclaim_idx')));
SELECT bm25_spill_index('memtable_reclaim_idx') IS NOT NULL AS mc_spill_5;
UPDATE multi_cycle_blocker_state
SET blocked_by_other_backend = blocked_by_other_backend
    OR other_backend_holds_xmin();
VACUUM ANALYZE memtable_reclaim_t;

SELECT max_live_pages > 1 AS multi_cycle_chain_multi_page
FROM multi_cycle_bounds;

-- Cumulative growth must stay within one live chain per cycle
-- (num_cycles * max_live_pages).  As above, skip when a blocker held the
-- horizon.
SELECT
    CASE
        WHEN (SELECT blocked_by_other_backend FROM multi_cycle_blocker_state)
            THEN true
        ELSE (pg_relation_size('memtable_reclaim_idx'::regclass, 'main')
              - (SELECT sz_start FROM multi_cycle_bounds))
             <= (SELECT num_cycles * max_live_pages FROM multi_cycle_bounds)
                * current_setting('block_size')::bigint
    END
    AS multi_cycle_growth_valid;

DROP FUNCTION other_backend_holds_xmin();
DROP TABLE memtable_reclaim_t;
DROP EXTENSION pg_textsearch CASCADE;
