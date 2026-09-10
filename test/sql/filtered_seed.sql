-- Test: selectivity-seeded top-K for filtered BM25 search
--
-- A filtered top-k query -- WHERE <filter> ORDER BY <score> LIMIT k --
-- is planned as a BM25 top-k index scan with <filter> applied as a
-- Filter above it.  pg_textsearch.filtered_seed seeds the scan's
-- internal top-K from the planner's estimated filter selectivity so a
-- single scoring pass usually surfaces enough matching rows, avoiding
-- the executor's backoff re-drives.  The seed only changes scan depth,
-- never which rows win, so results must be IDENTICAL to the un-seeded
-- plan.
--
-- This test asserts correctness, not latency:
--   * parity     - seeded result == un-seeded result (same code path,
--                  so immune to score ties)
--   * oracle     - the filtered index scan returns exactly the set of
--                  documents that match the filter AND contain a query
--                  term, checked against an independent plain-SQL regex
--                  (k large enough that the full match set is returned,
--                  so immune to ties and to score values)
--   * no-op      - a query with no filter is unaffected by seeding
--   * robustness - the result is independent of filtered_seed_margin
--   * GUC        - defaults and range enforcement

SET log_duration = off;
SET client_min_messages = WARNING; -- suppress index-build NOTICE chatter
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET enable_seqscan = false;

------------------------------------------------------------------------
-- Setup: 1000 docs, a 50-bucket facet column, controlled vocabulary.
--   'common' in every doc; 'alpha' in even ids; 'beta' every 3rd;
--   'gamma' every 7th; a unique 'doc<id>' token per row.
-- facet_id = id % 50  => 20 docs per bucket (2% selectivity each).
------------------------------------------------------------------------

CREATE TABLE fs_docs (
    id       int PRIMARY KEY,
    facet_id int,
    body     text
);

INSERT INTO fs_docs
SELECT g,
       g % 50,
       concat_ws(' ',
           'common',
           CASE WHEN g % 2 = 0 THEN 'alpha' END,
           CASE WHEN g % 3 = 0 THEN 'beta'  END,
           CASE WHEN g % 7 = 0 THEN 'gamma' END,
           'doc' || g)
FROM generate_series(1, 1000) g;

CREATE INDEX fs_docs_idx ON fs_docs USING bm25(body)
    WITH (text_config='english');

-- No index on facet_id: the facet is applied as a Filter above the
-- BM25 index scan, which is exactly the path filtered_seed optimizes.
-- (With a facet index the planner could instead scan the facet and
-- sort by score, bypassing the seed; that plan is equally correct but
-- not what this test exercises.)
ANALYZE fs_docs;

------------------------------------------------------------------------
-- Parity: seeded (default on) vs un-seeded top-k are identical.
-- Compared as sets (array_agg ORDER BY id); same index-scan code path
-- on both sides, so any score ties break identically.
------------------------------------------------------------------------

-- Confirm the plan under test: a BM25 index scan with the facet applied
-- as a Filter -- the path filtered_seed seeds.  If this ever becomes a
-- facet-index scan + sort, the checks below stop exercising the seed.
EXPLAIN (COSTS OFF)
SELECT id FROM fs_docs WHERE facet_id = 6
ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx')
LIMIT 10;

CREATE FUNCTION fs_check(qry text, pred text, k int) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    q        text;
    seeded   int[];
    unseeded int[];
BEGIN
    IF pred IS NULL THEN
        q := format(
            'SELECT coalesce(array_agg(id ORDER BY id), ARRAY[]::int[]) '
            'FROM (SELECT id FROM fs_docs '
            '      ORDER BY body <@> to_bm25query(%L, ''fs_docs_idx'') '
            '      LIMIT %s) s', qry, k);
    ELSE
        q := format(
            'SELECT coalesce(array_agg(id ORDER BY id), ARRAY[]::int[]) '
            'FROM (SELECT id FROM fs_docs WHERE %s '
            '      ORDER BY body <@> to_bm25query(%L, ''fs_docs_idx'') '
            '      LIMIT %s) s', pred, qry, k);
    END IF;

    SET LOCAL pg_textsearch.filtered_seed = on;
    EXECUTE q INTO seeded;
    SET LOCAL pg_textsearch.filtered_seed = off;
    EXECUTE q INTO unseeded;

    IF seeded IS NOT DISTINCT FROM unseeded THEN
        RETURN format('PASS (%s rows)', coalesce(array_length(seeded, 1), 0));
    END IF;
    RETURN format('FAIL seeded=%s unseeded=%s', seeded, unseeded);
END;
$$;

-- Independent oracle: the filtered index scan must return exactly the
-- documents matching the filter AND containing the query term.  k is
-- large enough (>= facet bucket size) that the whole match set is
-- returned, so this is immune to ranking, ties, and score values.
CREATE FUNCTION fs_oracle_check(qry text, term text, pred text, k int)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE
    idx int[];
    ora int[];
BEGIN
    EXECUTE format(
        'SELECT coalesce(array_agg(id ORDER BY id), ARRAY[]::int[]) '
        'FROM (SELECT id FROM fs_docs WHERE %s '
        '      ORDER BY body <@> to_bm25query(%L, ''fs_docs_idx'') '
        '      LIMIT %s) s', pred, qry, k)
    INTO idx;

    EXECUTE format(
        'SELECT coalesce(array_agg(id ORDER BY id), ARRAY[]::int[]) '
        'FROM fs_docs WHERE (%s) AND body ~ %L',
        pred, '\y' || term || '\y')
    INTO ora;

    IF idx IS NOT DISTINCT FROM ora THEN
        RETURN format('PASS (%s rows)', coalesce(array_length(idx, 1), 0));
    END IF;
    RETURN format('FAIL index=%s oracle=%s', idx, ora);
END;
$$;

-- Realistic filtered top-k (k < facet size, partial matches, ties).
SELECT n, qry, pred, fs_check(qry, pred, k) AS result
FROM (VALUES
    (1, 'alpha',      'facet_id = 6',            10),
    (2, 'beta',       'facet_id < 5',            10),
    (3, 'common',     'facet_id = 13',            5),
    (4, 'alpha beta', 'facet_id < 10',            8),
    (5, 'gamma',      'facet_id IN (0, 7, 14)',   6),
    (6, 'gamma',      'facet_id >= 45',           7)
) v(n, qry, pred, k)
ORDER BY n;

-- Cross-check the returned set against a plain-SQL regex oracle.
SELECT n, qry, pred, fs_oracle_check(qry, term, pred, k) AS result
FROM (VALUES
    (1, 'alpha',  'alpha',  'facet_id = 6',  1000),
    (2, 'gamma',  'gamma',  'facet_id = 9',  1000),
    (3, 'beta',   'beta',   'facet_id = 2',  1000),
    (4, 'common', 'common', 'facet_id = 40', 1000)
) v(n, qry, term, pred, k)
ORDER BY n;

------------------------------------------------------------------------
-- No-op: a query with no filter is unaffected by seeding.
------------------------------------------------------------------------

SELECT n, qry, fs_check(qry, NULL, k) AS result
FROM (VALUES
    (1, 'alpha',  10),
    (2, 'common',  5)
) v(n, qry, k)
ORDER BY n;

------------------------------------------------------------------------
-- Robustness: the result is independent of the margin (margin only
-- affects how deep the seeded pass scores, never the final top-k).
------------------------------------------------------------------------

SET pg_textsearch.filtered_seed_margin = 1.0;
SELECT 'margin=1.0'   AS margin, fs_check('alpha', 'facet_id = 6', 10) AS result;
SET pg_textsearch.filtered_seed_margin = 1000.0;
SELECT 'margin=1000' AS margin, fs_check('alpha', 'facet_id = 6', 10) AS result;
RESET pg_textsearch.filtered_seed_margin;

------------------------------------------------------------------------
-- GUC contract: defaults and range enforcement.
------------------------------------------------------------------------

SHOW pg_textsearch.filtered_seed;
SHOW pg_textsearch.filtered_seed_margin;

-- Out of range (min 1.0, max 1000.0) -- both should error.
SET pg_textsearch.filtered_seed_margin = 0.5;
SET pg_textsearch.filtered_seed_margin = 2000;

------------------------------------------------------------------------
-- Scan identity (issue #435): the seed is bound per index scan, so
-- several BM25 scans of one index in one statement each get the seed
-- derived from their own Filter and their own Limit.  Results are
-- identical either way -- Filter + Limit + backoff still decide the
-- top-k -- so these are parity checks; the depth assertions that
-- actually prove per-scan seeding are in the section after this one.
------------------------------------------------------------------------

-- Parity for an arbitrary query shape: run it with seeding on and off
-- and compare the results as a multiset.  q must yield a column named
-- id.
CREATE FUNCTION fs_check_q(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    wrapped  text;
    seeded   int[];
    unseeded int[];
BEGIN
    wrapped := format(
        'SELECT coalesce(array_agg(id ORDER BY id), ARRAY[]::int[]) '
        'FROM (%s) s', q);

    SET LOCAL pg_textsearch.filtered_seed = on;
    EXECUTE wrapped INTO seeded;
    SET LOCAL pg_textsearch.filtered_seed = off;
    EXECUTE wrapped INTO unseeded;

    IF seeded IS NOT DISTINCT FROM unseeded THEN
        RETURN format('PASS (%s rows)', coalesce(array_length(seeded, 1), 0));
    END IF;
    RETURN format('FAIL seeded=%s unseeded=%s', seeded, unseeded);
END;
$$;

SELECT n, what, fs_check_q(q) AS result
FROM (VALUES
    (1, 'UNION ALL, two facets, same LIMIT', $q$
        (SELECT id FROM fs_docs WHERE facet_id = 6
         ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10)
        UNION ALL
        (SELECT id FROM fs_docs WHERE facet_id = 12
         ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10)
    $q$),
    (2, 'UNION ALL, two different LIMITs', $q$
        (SELECT id FROM fs_docs WHERE facet_id = 6
         ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10)
        UNION ALL
        (SELECT id FROM fs_docs WHERE facet_id < 10
         ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 50)
    $q$),
    (3, 'self-join, two facets', $q$
        SELECT a.id AS id
        FROM (SELECT id FROM fs_docs WHERE facet_id = 6
              ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx')
              LIMIT 5) a
        CROSS JOIN (SELECT id FROM fs_docs WHERE facet_id = 12
                    ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx')
                    LIMIT 5) b
        WHERE b.id > a.id
    $q$),
    (4, 'CTE referenced twice', $q$
        WITH t AS (
            SELECT id FROM fs_docs WHERE facet_id = 6
            ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10)
        SELECT id FROM t UNION ALL SELECT id FROM t
    $q$),
    (5, 'CTE NOT MATERIALIZED referenced twice', $q$
        WITH t AS NOT MATERIALIZED (
            SELECT id FROM fs_docs WHERE facet_id = 6
            ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10)
        SELECT id FROM t UNION ALL SELECT id FROM t
    $q$),
    (6, 'unfiltered UNION ALL, two LIMITs', $q$
        (SELECT id FROM fs_docs
         ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10)
        UNION ALL
        (SELECT id FROM fs_docs
         ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 50)
    $q$),
    (7, 'LIMIT with OFFSET', $q$
        SELECT id FROM fs_docs WHERE facet_id < 10
        ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx')
        LIMIT 10 OFFSET 20
    $q$),
    (8, 'Sort above the BM25 subquery', $q$
        SELECT id FROM (
            SELECT id FROM fs_docs WHERE facet_id = 6
            ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx')) s
        ORDER BY id LIMIT 10
    $q$),
    (9, 'BM25 scan in a correlated subquery', $q$
        SELECT f.facet_id AS id FROM
            (SELECT DISTINCT facet_id FROM fs_docs WHERE facet_id < 4) f
        WHERE EXISTS (
            SELECT 1 FROM fs_docs d WHERE d.facet_id = f.facet_id
            ORDER BY d.body <@> to_bm25query('alpha', 'fs_docs_idx')
            LIMIT 3)
    $q$)
) v(n, what, q)
ORDER BY n;

-- Each arm of a two-arm UNION ALL must return what that arm returns
-- standalone.  Before #435 the two scans shared one per-index limit
-- slot, so one arm could run at the other's depth (or at the default).
CREATE FUNCTION fs_arms_check(q1 text, q2 text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    agg   text := 'coalesce(array_agg(id ORDER BY id), ARRAY[]::int[])';
    solo1 int[];
    solo2 int[];
    both1 int[] := ARRAY[]::int[];
    both2 int[] := ARRAY[]::int[];
    r     record;
BEGIN
    EXECUTE format('SELECT %s FROM (%s) s', agg, q1) INTO solo1;
    EXECUTE format('SELECT %s FROM (%s) s', agg, q2) INTO solo2;

    -- One statement, two scans of fs_docs_idx.  The GROUP BY sits
    -- above the UNION ALL so neither arm can be pruned away.
    FOR r IN EXECUTE format(
        'SELECT arm, %s AS ids FROM ('
        '    SELECT 1 AS arm, id FROM (%s) a '
        '    UNION ALL '
        '    SELECT 2 AS arm, id FROM (%s) b) u '
        'GROUP BY arm', agg, q1, q2)
    LOOP
        IF r.arm = 1 THEN both1 := r.ids; ELSE both2 := r.ids; END IF;
    END LOOP;

    IF both1 IS NOT DISTINCT FROM solo1 AND
       both2 IS NOT DISTINCT FROM solo2 THEN
        RETURN format('PASS (%s + %s rows)',
                      coalesce(array_length(solo1, 1), 0),
                      coalesce(array_length(solo2, 1), 0));
    END IF;
    RETURN format('FAIL arm1 both=%s solo=%s / arm2 both=%s solo=%s',
                  both1, solo1, both2, solo2);
END;
$$;

SELECT n, what, fs_arms_check(q1, q2) AS result
FROM (VALUES
    (1, 'two facets, same LIMIT',
     $q$SELECT id FROM fs_docs WHERE facet_id = 6
        ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10$q$,
     $q$SELECT id FROM fs_docs WHERE facet_id = 12
        ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10$q$),
    (2, 'two facets, two LIMITs',
     $q$SELECT id FROM fs_docs WHERE facet_id = 6
        ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10$q$,
     $q$SELECT id FROM fs_docs WHERE facet_id < 10
        ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 50$q$),
    (3, 'no filter, two LIMITs',
     $q$SELECT id FROM fs_docs
        ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10$q$,
     $q$SELECT id FROM fs_docs
        ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 50$q$)
) v(n, what, q1, q2)
ORDER BY n;

-- Cached plans: the seed is bound at every ExecutorStart, so the
-- custom-to-generic plan flip (which happens on the sixth EXECUTE)
-- and a margin change between executes must not alter the result.
PREPARE fs_union(int, int) AS
SELECT coalesce(array_agg(id ORDER BY id), ARRAY[]::int[]) FROM (
    (SELECT id FROM fs_docs WHERE facet_id = $1
     ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10)
    UNION ALL
    (SELECT id FROM fs_docs WHERE facet_id = $2
     ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10)) s;

EXECUTE fs_union(6, 12);
EXECUTE fs_union(6, 12);
EXECUTE fs_union(6, 12);
EXECUTE fs_union(6, 12);
EXECUTE fs_union(6, 12);
EXECUTE fs_union(6, 12);
EXECUTE fs_union(6, 12);
SET pg_textsearch.filtered_seed_margin = 50.0;
EXECUTE fs_union(6, 12);
RESET pg_textsearch.filtered_seed_margin;
EXECUTE fs_union(6, 12);
DEALLOCATE fs_union;

-- LIMIT $1: a PARAM_EXTERN limit is evaluated at bind time, so even a
-- generic plan seeds from the real k.  Declared bigint because that is
-- the type the parser coerces a LIMIT to; see the depth section.
PREPARE fs_lim(bigint) AS
SELECT coalesce(array_agg(id ORDER BY id), ARRAY[]::int[]) FROM (
    SELECT id FROM fs_docs WHERE facet_id = 6
    ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT $1) s;

EXECUTE fs_lim(10);
EXECUTE fs_lim(10);
EXECUTE fs_lim(10);
EXECUTE fs_lim(10);
EXECUTE fs_lim(10);
EXECUTE fs_lim(10);
EXECUTE fs_lim(10);
DEALLOCATE fs_lim;

-- Agg above a limited BM25 subquery.
SELECT count(*) AS agg_rows FROM (
    SELECT id FROM fs_docs WHERE facet_id = 6
    ORDER BY body <@> to_bm25query('alpha', 'fs_docs_idx') LIMIT 10) s;

------------------------------------------------------------------------
-- Depth: the acceptance test for issue #435.
--
-- Scan depth is invisible in results, so the parity checks above pass
-- with or without the fix.  bm25_debug_scoring_passes counts scoring
-- passes: a scan seeded for its own filter costs exactly one, and each
-- executor backoff re-drive adds another.
--
-- Both arms below filter down to fewer than k rows, so the executor
-- always exhausts the batch and backs off -- the pass count is then a
-- function of the seed alone, not of score ties.  With a correct
-- per-scan seed each arm costs one pass; when one arm steals the
-- other's seed and the loser falls back to pg_textsearch.default_limit,
-- the loser pays a chain of doublings instead.
------------------------------------------------------------------------

CREATE FUNCTION fs_passes(q text) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    n bigint;
BEGIN
    PERFORM bm25_debug_scoring_passes(true);
    EXECUTE q;
    SELECT bm25_debug_scoring_passes() INTO n;
    RETURN n;
END;
$$;

-- A small default_limit lengthens the backoff chain an unseeded arm
-- pays, so the two cases are far apart rather than adjacent.
SET pg_textsearch.default_limit = 100;

-- Confirm the plan under test: two BM25 index scans of one index, each
-- under its own Limit and with its own Filter.  Counted rather than
-- printed, so subquery aliases and node order cannot make this brittle.
CREATE FUNCTION fs_plan_shape(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    r       record;
    scans   int := 0;
    limits  int := 0;
    filters int := 0;
BEGIN
    FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
        IF r."QUERY PLAN" LIKE '%Index Scan using fs_docs_idx%' THEN
            scans := scans + 1;
        ELSIF r."QUERY PLAN" LIKE '%Limit%' THEN
            limits := limits + 1;
        ELSIF r."QUERY PLAN" LIKE '%Filter:%' THEN
            filters := filters + 1;
        END IF;
    END LOOP;
    RETURN format('%s bm25 scans, %s limits, %s filters',
                  scans, limits, filters);
END;
$$;

SELECT fs_plan_shape($q$
    (SELECT id FROM fs_docs WHERE facet_id = 6 AND id % 7 = 3
     ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT 10)
    UNION ALL
    (SELECT id FROM fs_docs WHERE facet_id = 13 AND id % 7 = 3
     ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT 10)
$q$) AS plan_shape;

-- Control: one scan, one Limit, one Filter -- one pass.  This holds
-- with or without the fix; it calibrates the counter.
SELECT fs_passes($q$
    SELECT id FROM fs_docs WHERE facet_id = 6 AND id % 7 = 3
    ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT 10
$q$) AS single_scan_passes;

-- The issue #435 example: two scans of one index, each with its own
-- Filter.  Must be one pass per arm.
SELECT fs_passes($q$
    (SELECT id FROM fs_docs WHERE facet_id = 6 AND id % 7 = 3
     ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT 10)
    UNION ALL
    (SELECT id FROM fs_docs WHERE facet_id = 13 AND id % 7 = 3
     ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT 10)
$q$) AS union_all_passes;

-- Three arms, to show the count tracks the number of scans rather
-- than happening to be small.
SELECT fs_passes($q$
    (SELECT id FROM fs_docs WHERE facet_id = 6 AND id % 7 = 3
     ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT 10)
    UNION ALL
    (SELECT id FROM fs_docs WHERE facet_id = 13 AND id % 7 = 3
     ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT 10)
    UNION ALL
    (SELECT id FROM fs_docs WHERE facet_id = 21 AND id % 7 = 3
     ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT 10)
$q$) AS three_arm_passes;

-- A correlated SubPlan is walked as its own root with the Limit inside
-- it, so it is seeded.  Every rescan hands tp_rescan the same ScanKey
-- array, so the seed is restored on each of the three evaluations
-- rather than only the first.
SELECT fs_passes($q$
    SELECT sum(c) FROM (
        SELECT (SELECT count(*) FROM (
                    SELECT id FROM fs_docs
                    WHERE facet_id = v.fid AND id % 7 = 3
                    ORDER BY body <@> to_bm25query('common', 'fs_docs_idx')
                    LIMIT 10) x) AS c
        FROM (VALUES (6), (13), (21)) v(fid)) y
$q$) AS correlated_subplan_passes;

-- A generic plan's LIMIT is a PARAM_EXTERN, evaluated at bind time, so
-- it seeds where plan-time seeding could not fold the Param at all.
-- The parser coerces a LIMIT to bigint, so only a bigint parameter
-- reaches the Limit node as a bare Param: an int parameter arrives
-- wrapped in an int8() coercion, which is not something we evaluate at
-- bind time, and that scan falls back to default_limit plus backoff.
PREPARE fs_dp_big(bigint) AS
    SELECT id FROM fs_docs WHERE facet_id = 6 AND id % 7 = 3
    ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT $1;
PREPARE fs_dp_int(int) AS
    SELECT id FROM fs_docs WHERE facet_id = 6 AND id % 7 = 3
    ORDER BY body <@> to_bm25query('common', 'fs_docs_idx') LIMIT $1;

-- Runs stmt n times first, so the plan has flipped from custom to
-- generic before the measured execution.
CREATE FUNCTION fs_generic_passes(stmt text, n int) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE
    p bigint;
BEGIN
    FOR i IN 1..n LOOP EXECUTE stmt; END LOOP;
    PERFORM bm25_debug_scoring_passes(true);
    EXECUTE stmt;
    SELECT bm25_debug_scoring_passes() INTO p;
    RETURN p;
END;
$$;

SELECT fs_generic_passes('EXECUTE fs_dp_big(10)', 6) AS bigint_param_passes;
SELECT fs_generic_passes('EXECUTE fs_dp_int(10)', 6) AS int_param_passes;

DEALLOCATE fs_dp_big;
DEALLOCATE fs_dp_int;
RESET pg_textsearch.default_limit;

------------------------------------------------------------------------
-- Saturating LIMIT: offset + count at or above INT_MAX yields no k.
--
-- The seed becomes so->limit, which sizes a palloc of k
-- ItemPointerDatas, so a k of INT_MAX asks for 12 GB and the query
-- dies with "invalid memory alloc request size".  LIMIT 2147483647 is
-- the usual generated-SQL spelling of "no limit", so these must fall
-- back to the default rather than bind a k that cannot be served.
------------------------------------------------------------------------

-- 20 docs per facet bucket; each query must return the whole bucket.
SELECT count(*) AS int_max_limit FROM (
    SELECT id FROM fs_docs WHERE facet_id = 6
    ORDER BY body <@> to_bm25query('common', 'fs_docs_idx')
    LIMIT 2147483647) s;

SELECT count(*) AS count_over_int_max FROM (
    SELECT id FROM fs_docs WHERE facet_id = 6
    ORDER BY body <@> to_bm25query('common', 'fs_docs_idx')
    LIMIT 9999999999) s;

-- k is offset + count, so a large OFFSET saturates it just as a large
-- count does.  Nothing survives the offset; the point is that it runs.
SELECT count(*) AS int_max_offset FROM (
    SELECT id FROM fs_docs WHERE facet_id = 6
    ORDER BY body <@> to_bm25query('common', 'fs_docs_idx')
    LIMIT 10 OFFSET 3000000000) s;

-- A k that binds normally, so the cases above are shown to be about
-- saturation and not about large limits in general.
SELECT count(*) AS large_bound_limit FROM (
    SELECT id FROM fs_docs WHERE facet_id = 6
    ORDER BY body <@> to_bm25query('common', 'fs_docs_idx')
    LIMIT 1000000) s;

------------------------------------------------------------------------
-- Cleanup
------------------------------------------------------------------------
DROP FUNCTION fs_check(text, text, int);
DROP FUNCTION fs_oracle_check(text, text, text, int);
DROP FUNCTION fs_check_q(text);
DROP FUNCTION fs_arms_check(text, text);
DROP FUNCTION fs_plan_shape(text);
DROP FUNCTION fs_passes(text);
DROP FUNCTION fs_generic_passes(text, int);
DROP TABLE fs_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
