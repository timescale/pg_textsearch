-- Test unlogged index behavior and initialization fork creation
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create UNLOGGED table and index
CREATE UNLOGGED TABLE unlogged_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO unlogged_test (content) VALUES ('test document');

CREATE INDEX unlogged_idx ON unlogged_test USING bm25(content) 
WITH (text_config='english', k1=1.2, b=0.75);

-- VERIFICATION: Check if _init fork exists and has valid size
--
-- We verify that the init fork contains at least the meta page
SELECT 
    relname, 
    -- Main fork should have data (Metapage + DocID page + etc)
    pg_relation_size(indexrelid, 'main') > current_setting('block_size')::int as main_has_data,
    -- Init fork must exist and be at least one page size
    CASE 
        WHEN pg_relation_size(indexrelid, 'init') >= current_setting('block_size')::int 
        THEN 'PASS' 
        ELSE 'FAIL' 
    END as init_check
FROM pg_index i
JOIN pg_class c ON c.oid = i.indexrelid
WHERE c.relname = 'unlogged_idx';

-- ----------------------------------------------------------------
-- Coverage for level-N → level-(N+1) merge on an UNLOGGED index
--
-- merge.c uses GenericXLog for the linkage update on both
-- WAL-logged and UNLOGGED / TEMP indexes (issue #374 unified
-- the paths).  The UNLOGGED case is otherwise unexercised
-- by the regression suite -- almost every other test runs against
-- WAL-logged indexes -- so drive a real compaction here.
--
-- Set segments_per_level = 2 so 3 spills cross the threshold and
-- the third spill triggers a level-0 → level-1 merge.
-- ----------------------------------------------------------------
CREATE UNLOGGED TABLE unlogged_merge (
    id SERIAL PRIMARY KEY,
    content TEXT
);
CREATE INDEX unlogged_merge_idx ON unlogged_merge USING bm25(content)
    WITH (text_config='english');

SET pg_textsearch.segments_per_level = 2;

-- Three explicit spills, all routed through the GenericXLog
-- linkage path because the index is UNLOGGED.
INSERT INTO unlogged_merge (content)
    SELECT 'alpha bravo doc ' || i FROM generate_series(1, 50) i;
SELECT bm25_spill_index('unlogged_merge_idx') > 0 AS spill1_wrote;

INSERT INTO unlogged_merge (content)
    SELECT 'charlie delta doc ' || i FROM generate_series(51, 100) i;
SELECT bm25_spill_index('unlogged_merge_idx') > 0 AS spill2_wrote;

-- Third spill should trigger compaction of L0 → L1 via the
-- GenericXLog merge fallback (covers merge.c's else-branch).
INSERT INTO unlogged_merge (content)
    SELECT 'echo foxtrot doc ' || i FROM generate_series(101, 150) i;
SELECT bm25_spill_index('unlogged_merge_idx') > 0 AS spill3_wrote;

-- Verify the merge happened: bm25_summarize_index should report
-- at least one segment at level 1 (post-merge target). If the
-- merge didn't run, all segments would still be at level 0.
SELECT bm25_summarize_index('unlogged_merge_idx') ~ 'L1 Segment'
    AS l1_has_segment;

-- Sanity: a query against the merged corpus still returns rows.
SELECT count(*) > 0 AS query_returns_rows
FROM (
    SELECT id FROM unlogged_merge
    ORDER BY content <@> to_bm25query('alpha', 'unlogged_merge_idx')
    LIMIT 10000
) t;

RESET pg_textsearch.segments_per_level;

-- ----------------------------------------------------------------
-- Coverage for shrinkage in the GenericXLog merge fallback
--
-- The else-branch of merge.c also has to subtract dead-doc count
-- and total_tokens from the in-memory atomics and the on-disk
-- metapage when V5 alive-bitset shrinkage drops docs at merge
-- time. Without a delete+vacuum sequence the shrinkage values
-- are zero and the `if (docs_shrinkage > 0)` / `if
-- (tokens_shrinkage > 0)` branches stay uncovered. Drive a
-- clean delete + VACUUM + force-merge here to exercise them.
--
-- Run with the default segments_per_level (= 8) so the L0 spills
-- here don't auto-compact: we want the merge to run only via the
-- explicit force-merge call after the alive-bitset bits have been
-- flipped dead by VACUUM.
--
-- HISTORY / FLAKE INSTRUMENTATION (issue #397).
--
-- The post-merge docs_persisted assertion has flaked on the PG17/18
-- Sanitizer jobs twice now:
--   * first against the legacy `total_docs:` shared-memory atomic
--     (fixed in 65b77c52 by switching the assertion to the metapage
--     value),
--   * then against the new `docs_persisted:` metapage assertion
--     itself (issue #397).
--
-- We genuinely do not know what value `docs_persisted` reaches on
-- a failure -- the original opaque `~ 'docs_persisted: 100'` regex
-- assertion just flipped `t` -> `f`.  To diagnose, this section
-- now runs the post-VACUUM force-merge cycle in a 64-iteration
-- loop and captures the actual `docs_persisted` / `len_persisted`
-- values plus per-segment header state into a temp table.  On the
-- happy path every iteration emits identical output; on a flake
-- the regression diff pinpoints the iteration and the wrong value.
--
-- The loop count is sized to give ~99% odds of catching a 5% flake
-- rate in a single CI pass while keeping wall time below ~10s.
-- ----------------------------------------------------------------

CREATE TABLE unlogged_shrink_diag (
    iter             int  PRIMARY KEY,
    docs_persisted   text NOT NULL,
    len_persisted    text NOT NULL,
    segments_summary text NOT NULL
);

-- Drive the per-iteration workload via \gexec.  Each iteration is
-- emitted as multiple rows (one statement per row) so \gexec sends
-- each statement as a separate PQexec call -- otherwise psql would
-- batch them into one simple-query, which implicitly wraps the
-- block in a transaction and rejects VACUUM.  Output of the
-- generator query and of every executed iteration is suppressed so
-- the regression .out file stays focused on the final diagnostic
-- SELECTs.
\set ECHO none
\o /dev/null
SET client_min_messages = warning;

WITH steps(stepno, sql_template) AS (VALUES
    (1,  $$DROP TABLE IF EXISTS unlogged_shrink CASCADE$$),
    (2,  $$CREATE UNLOGGED TABLE unlogged_shrink (
              id      SERIAL PRIMARY KEY,
              content TEXT
          )$$),
    (3,  $$CREATE INDEX unlogged_shrink_idx
              ON unlogged_shrink USING bm25(content)
              WITH (text_config='english')$$),
    (4,  $$INSERT INTO unlogged_shrink (content)
              SELECT 'foo doc %1$s ' || i FROM generate_series(1, 100) i$$),
    (5,  $$SELECT bm25_spill_index('unlogged_shrink_idx')$$),
    (6,  $$INSERT INTO unlogged_shrink (content)
              SELECT 'bar doc %1$s ' || i FROM generate_series(101, 200) i$$),
    (7,  $$SELECT bm25_spill_index('unlogged_shrink_idx')$$),
    (8,  $$DELETE FROM unlogged_shrink
              WHERE id <= 50 OR (id > 100 AND id <= 150)$$),
    (9,  $$VACUUM unlogged_shrink$$),
    (10, $$SELECT bm25_force_merge('unlogged_shrink_idx')$$),
    (11, $$INSERT INTO unlogged_shrink_diag (iter, docs_persisted,
                                            len_persisted, segments_summary)
          SELECT %1$s,
                 COALESCE(substring(s from 'docs_persisted: \d+'),
                          '<MISSING docs_persisted>'),
                 COALESCE(substring(s from 'len_persisted: \d+'),
                          '<MISSING len_persisted>'),
                 COALESCE(NULLIF(
                            array_to_string(
                              ARRAY(
                                SELECT regexp_replace(
                                         regexp_replace(line,
                                           'block=\d+, pages=\d+, size=[0-9.]+MB, ',
                                           '', 'g'),
                                         '\s+', ' ', 'g')
                                FROM unnest(string_to_array(s, E'\n')) AS t(line)
                                WHERE line ~ '^\s*L\d+ Segment '
                                ORDER BY line
                              ),
                              E'\n'),
                            ''),
                          '<NO SEGMENTS>')
          FROM (SELECT bm25_summarize_index('unlogged_shrink_idx') AS s) sub$$)
)
SELECT format(sql_template, n)
FROM generate_series(1, 64) n, steps
ORDER BY n, stepno
\gexec

RESET client_min_messages;
\o
\set ECHO queries

-- Deterministic per-iteration diagnostics.  On the happy path
-- every row is identical (docs_persisted: 100, single L1 segment
-- with 100 docs).  On a flake the regression.diffs file
-- pinpoints the exact iteration and value that drifted.
SELECT iter, docs_persisted, len_persisted, segments_summary
FROM unlogged_shrink_diag
ORDER BY iter;

-- Distinct-value collapse: one row on the happy path, multiple
-- on a flake.  Useful for at-a-glance scanning when the loop
-- count grows large.
SELECT docs_persisted, len_persisted, segments_summary, count(*) AS n_iter
FROM unlogged_shrink_diag
GROUP BY docs_persisted, len_persisted, segments_summary
ORDER BY docs_persisted, len_persisted, segments_summary;

-- Cleanup
DROP TABLE unlogged_shrink_diag;
DROP TABLE unlogged_shrink;
DROP TABLE unlogged_merge;
DROP TABLE unlogged_test;
DROP EXTENSION pg_textsearch CASCADE;