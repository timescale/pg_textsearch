-- Test: selectivity-seeded top-K for filtered BM25 search
--
-- A filtered top-k query -- WHERE <filter> ORDER BY <score> LIMIT k --
-- is planned as a BM25 top-k index scan with <filter> applied as a
-- Filter above it.  pg_textsearch.filtered_seed seeds the scan's
-- internal top-K from the planner's estimated filter selectivity so a
-- single scoring pass usually surfaces enough matching rows, avoiding
-- the executor's backoff re-drives.  The seed only changes scan depth,
-- never which rows win, so results must be IDENTICAL to the un-seeded
-- plan.  See docs/filtered_topk_seed.md.
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
-- Cleanup
------------------------------------------------------------------------
DROP FUNCTION fs_check(text, text, int);
DROP FUNCTION fs_oracle_check(text, text, text, int);
DROP TABLE fs_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
