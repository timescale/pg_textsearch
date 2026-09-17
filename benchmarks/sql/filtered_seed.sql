-- Filtered-seed top-K benchmark (issues #434, #435)
--
-- A filtered top-k query -- WHERE <filter> ORDER BY <score> LIMIT k --
-- is planned as a BM25 top-k index scan with <filter> applied as a
-- Filter above it.  If the scan surfaces only its top k rows by score,
-- few may survive the Filter, and the executor re-drives the scan with
-- a doubling internal limit until k rows do.  Each re-drive re-walks
-- the posting lists from the start.
--
-- pg_textsearch.filtered_seed seeds the scan's internal top-K from the
-- planner's estimated filter selectivity so one pass usually suffices.
-- This benchmark measures what that is worth, by toggling the GUC over
-- identical data and queries.
--
-- Two query shapes, because they exercise different fixes:
--
--   single  - one BM25 scan.  This is #434, which shipped earlier.
--   union2  - two or three BM25 scans of ONE index in one statement,
--   union3    each with its own Filter.  This is #435: the seed is
--             bound per scan, keyed by scan identity.  Before that fix
--             one arm consumed the single per-index slot and the rest
--             fell back to pg_textsearch.default_limit plus backoff,
--             so the 'on' numbers for these shapes are only achievable
--             with #435.
--
-- To measure what #435 itself bought, run this against a build that
-- predates it and compare the **seed on** columns only.  The seed off
-- columns are not a stable cross-version baseline: with #435 the k
-- binding is per scan even when seeding is disabled, so 'off' means
-- "every arm capped at its own k" on a new build versus "one arm at
-- k, the rest at default_limit" on an old one.  Those are different
-- amounts of work, and neither is the thing being measured.
--
-- Results are identical either way -- seeding changes scan depth, not
-- which rows win -- so the metrics are latency and scoring passes.
-- bm25_debug_scoring_passes counts passes: a well-seeded scan costs
-- exactly one, and each backoff re-drive adds another.
--
-- Usage:
--   psql -f filtered_seed.sql                    -- 200k docs
--   psql -v ndocs=1000000 -f filtered_seed.sql

\if :{?ndocs}
\else
  \set ndocs 200000
\endif

\set ON_ERROR_STOP on
\timing off
SET client_min_messages = WARNING;

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\echo ''
\echo '=== Filtered-seed top-K benchmark ==='
\echo ''

DROP TABLE IF EXISTS fsb_docs CASCADE;

CREATE TABLE fsb_docs (
    id    int PRIMARY KEY,
    facet int,
    body  text
);

-- 'common' is in every document, so its posting list spans the whole
-- corpus and scan depth is what costs.  facet = id % 1000 is uniform,
-- giving predictable selectivity that the planner estimates well: this
-- measures seeding, not estimation error.
INSERT INTO fsb_docs
SELECT g,
       g % 1000,
       concat_ws(' ',
           'common',
           CASE WHEN g % 2 = 0 THEN 'alpha' END,
           CASE WHEN g % 3 = 0 THEN 'beta'  END,
           CASE WHEN g % 7 = 0 THEN 'gamma' END,
           'doc' || g)
FROM generate_series(1, :ndocs) g;

CREATE INDEX fsb_idx ON fsb_docs USING bm25(body)
    WITH (text_config='english');

-- No index on facet: the facet is applied as a Filter above the BM25
-- index scan, which is the path filtered_seed optimizes.
ANALYZE fsb_docs;

\echo 'Corpus:'
SELECT count(*) AS docs,
       count(DISTINCT facet) AS facet_buckets,
       pg_size_pretty(pg_relation_size('fsb_idx')) AS index_size
FROM fsb_docs;

------------------------------------------------------------------------
-- Harness
------------------------------------------------------------------------

CREATE TABLE fsb_results (
    shape      text,
    sel        text,
    lim        int,
    scans      int,
    off_ms     numeric,
    off_passes bigint,
    on_ms      numeric,
    on_passes  bigint
);

-- bm25_debug_scoring_passes is newer than the seeding itself, so it is
-- absent on older builds.  Returning NULL there keeps the benchmark
-- runnable against a version that predates the counter, which is what
-- makes the main-versus-branch comparison in the header possible.
-- Never called inside a timed loop, so the subtransaction is free.
CREATE FUNCTION fsb_passes(reset boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
BEGIN
    RETURN bm25_debug_scoring_passes(reset);
EXCEPTION
    WHEN undefined_function THEN RETURN NULL;
END;
$$;

-- How many BM25 index scans of fsb_idx the plan actually contains.
-- Zero means the planner chose a seq scan and sort, and the cell says
-- nothing about seeding.  Read from EXPLAIN rather than inferred from
-- the pass counter, so the check works on builds without the counter.
CREATE FUNCTION fsb_scan_count(q text) RETURNS int
LANGUAGE plpgsql AS $$
DECLARE
    r record;
    n int := 0;
BEGIN
    FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
        IF r."QUERY PLAN" LIKE '%Index Scan using fsb_idx%' THEN
            n := n + 1;
        END IF;
    END LOOP;
    RETURN n;
END;
$$;

-- Median of `iters` timed runs, plus the scoring-pass count from one
-- separate run.  A warmup run first, so the numbers are steady-state
-- and not dominated by first-touch buffer reads.
CREATE FUNCTION fsb_measure(q text, iters int DEFAULT 7)
RETURNS TABLE (median_ms numeric, passes bigint)
LANGUAGE plpgsql AS $$
DECLARE
    t0      timestamptz;
    samples double precision[] := '{}';
    i       int;
BEGIN
    EXECUTE q;

    PERFORM fsb_passes(true);
    EXECUTE q;
    passes := fsb_passes(true);

    FOR i IN 1..iters LOOP
        t0 := clock_timestamp();
        EXECUTE q;
        samples := samples ||
            (EXTRACT(epoch FROM clock_timestamp() - t0) * 1000.0);
    END LOOP;

    SELECT round(percentile_cont(0.5) WITHIN GROUP (ORDER BY s)::numeric, 2)
      INTO median_ms
      FROM unnest(samples) s;

    RETURN NEXT;
END;
$$;

-- Measure one query with seeding off, then on, and record the pair.
CREATE FUNCTION fsb_run(shape text, sel text, lim int, q text)
RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
    off_ms numeric;
    off_p  bigint;
    on_ms  numeric;
    on_p   bigint;
    scans  int;
BEGIN
    /*
     * Loud, because a plausible-looking latency pair from a plan that
     * has no BM25 scan in it is worse than no row at all.
     */
    scans := fsb_scan_count(q);
    IF scans = 0 THEN
        RAISE WARNING 'no BM25 scan for shape=% sel=% limit=%; '
                      'planner chose another plan, cell dropped',
                      shape, sel, lim;
        RETURN;
    END IF;

    SET pg_textsearch.filtered_seed = off;
    SELECT median_ms, passes INTO off_ms, off_p FROM fsb_measure(q);

    SET pg_textsearch.filtered_seed = on;
    SELECT median_ms, passes INTO on_ms, on_p FROM fsb_measure(q);

    INSERT INTO fsb_results
    VALUES (shape, sel, lim, scans, off_ms, off_p, on_ms, on_p);
END;
$$;

-- The query shapes.  count(*) over a subquery consumes every row the
-- Limit produces without printing them.
CREATE FUNCTION fsb_q_single(pred text, lim int)
RETURNS text LANGUAGE sql IMMUTABLE AS $$
    SELECT format($f$
        SELECT count(*) FROM (
            SELECT id FROM fsb_docs WHERE %s
            ORDER BY body <@> to_bm25query('common', 'fsb_idx')
            LIMIT %s) s $f$, pred, lim);
$$;

CREATE FUNCTION fsb_q_union(preds text[], lim int)
RETURNS text LANGUAGE sql IMMUTABLE AS $$
    SELECT 'SELECT count(*) FROM (' || string_agg(
        format($f$(SELECT id FROM fsb_docs WHERE %s
                   ORDER BY body <@> to_bm25query('common', 'fsb_idx')
                   LIMIT %s)$f$, p, lim),
        ' UNION ALL ') || ') s'
    FROM unnest(preds) p;
$$;

------------------------------------------------------------------------
-- Plan confirmation: every cell below must be a BM25 index scan with a
-- Filter, or the benchmark is measuring the wrong thing.
------------------------------------------------------------------------

\echo ''
\echo 'Plan under test (single, sel=0.001):'
EXPLAIN (COSTS OFF)
SELECT id FROM fsb_docs WHERE facet = 7
ORDER BY body <@> to_bm25query('common', 'fsb_idx') LIMIT 10;

------------------------------------------------------------------------
-- Measurements
------------------------------------------------------------------------

\echo ''
\echo 'Measuring...'

DO $$
BEGIN
    /*
     * Selectivity is what drives the gain: the lower it is, the deeper
     * a scan must go to surface k survivors, and the longer the
     * backoff chain an unseeded scan pays to get there.
     */
    PERFORM fsb_run('single', '0.1',   10, fsb_q_single('facet < 100', 10));
    PERFORM fsb_run('single', '0.01',  10, fsb_q_single('facet < 10', 10));
    PERFORM fsb_run('single', '0.001', 10, fsb_q_single('facet = 7', 10));
    PERFORM fsb_run('single', '0.001', 100, fsb_q_single('facet = 7', 100));

    /* #435: several BM25 scans of one index in one statement. */
    PERFORM fsb_run('union2', '0.001', 10,
        fsb_q_union(ARRAY['facet = 7', 'facet = 13'], 10));
    PERFORM fsb_run('union3', '0.001', 10,
        fsb_q_union(ARRAY['facet = 7', 'facet = 13', 'facet = 29'], 10));
    /*
     * Arms with different selectivities, which is the case #435 is
     * really about: each arm needs a seed computed from its own
     * Filter, not whichever arm happened to reach the slot first.
     */
    PERFORM fsb_run('union3', 'mixed', 10,
        fsb_q_union(ARRAY['facet < 100', 'facet < 10', 'facet = 500'], 10));
END;
$$;

RESET pg_textsearch.filtered_seed;

------------------------------------------------------------------------
-- Report
------------------------------------------------------------------------

\echo ''
\echo 'Results:'
\pset border 2

SELECT shape,
       sel                                 AS selectivity,
       lim                                 AS "limit",
       scans                               AS "bm25 scans",
       off_ms                              AS "seed off (ms)",
       on_ms                               AS "seed on (ms)",
       round(off_ms / nullif(on_ms, 0), 2) AS speedup,
       off_passes                          AS "passes off",
       on_passes                           AS "passes on"
FROM fsb_results
ORDER BY shape, sel, lim;

\echo ''
\echo 'Parseable:'
\pset tuples_only on
\pset format unaligned

SELECT format(
    'FSB_RESULT: shape=%s sel=%s limit=%s scans=%s '
    'off_ms=%s off_passes=%s on_ms=%s on_passes=%s speedup=%s',
    shape, sel, lim, scans,
    off_ms, coalesce(off_passes::text, 'null'),
    on_ms, coalesce(on_passes::text, 'null'),
    round(off_ms / nullif(on_ms, 0), 2))
FROM fsb_results
ORDER BY shape, sel, lim;

\pset tuples_only off
\pset format aligned

------------------------------------------------------------------------
-- Cleanup
------------------------------------------------------------------------
DROP FUNCTION fsb_passes(boolean);
DROP FUNCTION fsb_scan_count(text);
DROP FUNCTION fsb_measure(text, int);
DROP FUNCTION fsb_run(text, text, int, text);
DROP FUNCTION fsb_q_single(text, int);
DROP FUNCTION fsb_q_union(text[], int);
DROP TABLE fsb_results;
DROP TABLE fsb_docs CASCADE;

\echo ''
\echo 'Done.'
