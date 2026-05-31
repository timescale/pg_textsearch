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
-- HISTORY / FIX (issue #397).
--
-- The post-merge docs_persisted assertion has flaked on the PG17/18
-- Sanitizer jobs twice now:
--   * first against the legacy `total_docs:` shared-memory atomic
--     (fixed in 65b77c52 by switching the assertion to the metapage
--     value),
--   * then against the new `docs_persisted:` metapage assertion
--     itself (issue #397).
--
-- Root cause of the #397 flake: every iteration `DROP TABLE` +
-- `CREATE UNLOGGED TABLE` + `CREATE INDEX` churns pg_catalog
-- (pg_class, pg_attribute, pg_shdepend, ...).  That triggers
-- autovacuum workers whose `backend_xmin` holds back the cluster's
-- `OldestNonRemovableTransactionId`.  If our `VACUUM
-- unlogged_shrink` runs while such a worker is active, the index
-- ambulkdelete callback classifies our DELETE'd tuples as "dead but
-- not yet removable", the per-segment alive_bitset never shrinks,
-- and `bm25_force_merge` produces a merged segment with all 200
-- source docs instead of the expected 100.  Symptom: a clean 2x
-- drift (`docs_persisted: 200` and merged-segment `docs=200`).
--
-- The diagnostic loop captured exactly this signature when the
-- flake fired locally, with the wait DO block below reporting
-- `[pid=N,type=autovacuum worker,query=autovacuum: VACUUM ANALYZE
-- pg_catalog.pg_shdepend]` as the xmin holder.
--
-- Fix: run the post-DELETE workload with a guarded VACUUM cycle:
--
--   (a) Wait briefly for any backend with a held xmin to finish.
--       Soft timeout (no error) so the loop keeps making forward
--       progress even on a heavily-churning cluster.
--   (b) Run `VACUUM unlogged_shrink`.
--   (c) Wait again, then run `VACUUM` a second time.  This closes
--       the residual race where a new autovacuum worker spawns
--       between the (a) wait-exit and the (b) VACUUM's OldestXmin
--       computation.  When (b) already shrank the bitset, the
--       second VACUUM is a cheap no-op.
--
-- The loop count is 8 -- small enough to keep CI wall time short
-- and to limit our own catalog churn, but large enough to amplify
-- any residual race a single-iteration assertion would miss.  Per
-- iteration we capture three diagnostic columns:
--
--   pre_merge_segs   -- scrubbed segment summary AFTER VACUUM,
--                       BEFORE force_merge.  Happy path shows
--                       both L0 segments annotated `(alive=50,
--                       dead=50)`; on a stale-bitset flake both
--                       would still report alive=100.
--   docs_persisted   -- metapage total_docs after merge (must
--                       be `docs_persisted: 100`).
--   post_merge_segs  -- scrubbed segment summary AFTER merge
--                       (must be one L1 segment with docs=100).
-- ----------------------------------------------------------------

CREATE TABLE unlogged_shrink_diag (
    iter             int  PRIMARY KEY,
    pre_merge_segs   text,
    docs_persisted   text,
    post_merge_segs  text
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
          ) WITH (autovacuum_enabled = false,
                  toast.autovacuum_enabled = false)$$),
    (3,  $$CREATE INDEX unlogged_shrink_idx
              ON unlogged_shrink USING bm25(content)
              WITH (text_config='english')$$),
    (4,  $$INSERT INTO unlogged_shrink (content)
              SELECT 'foo doc ' || i FROM generate_series(1, 100) i$$),
    (5,  $$SELECT bm25_spill_index('unlogged_shrink_idx')$$),
    (6,  $$INSERT INTO unlogged_shrink (content)
              SELECT 'bar doc ' || i FROM generate_series(101, 200) i$$),
    (7,  $$SELECT bm25_spill_index('unlogged_shrink_idx')$$),
    (8,  $$DELETE FROM unlogged_shrink
              WHERE id <= 50 OR (id > 100 AND id <= 150)$$),
    -- Issue #397 workaround: wait for any backend with a held
    -- backend_xmin to finish, then VACUUM, then wait+VACUUM a
    -- second time to close the residual race where a fresh
    -- autovacuum worker spawns between wait-exit and our VACUUM's
    -- OldestXmin computation.  Both waits use a soft 30 s cap
    -- (RAISE NOTICE on timeout, not RAISE EXCEPTION) so that on a
    -- heavily-churning dev box the loop keeps making forward
    -- progress and the final assertion is the source of truth.
    -- See the comment block above for full root-cause notes.
    (9,  $$DO $WAIT$
          DECLARE
              start_time timestamptz := clock_timestamp();
              blockers   int;
          BEGIN
              LOOP
                  SELECT count(*) INTO blockers
                  FROM pg_stat_activity
                  WHERE backend_xmin IS NOT NULL
                    AND pid <> pg_backend_pid()
                    AND backend_type <> 'walsender';
                  EXIT WHEN blockers = 0;
                  IF clock_timestamp() - start_time > interval '30 seconds' THEN
                      RAISE NOTICE
                          'wait for backend_xmin timed out after 30 s';
                      EXIT;
                  END IF;
                  PERFORM pg_sleep(0.02);
              END LOOP;
          END $WAIT$
          $$),
    (10, $$VACUUM unlogged_shrink$$),
    (11, $$DO $WAIT$
          DECLARE
              start_time timestamptz := clock_timestamp();
              blockers   int;
          BEGIN
              LOOP
                  SELECT count(*) INTO blockers
                  FROM pg_stat_activity
                  WHERE backend_xmin IS NOT NULL
                    AND pid <> pg_backend_pid()
                    AND backend_type <> 'walsender';
                  EXIT WHEN blockers = 0;
                  IF clock_timestamp() - start_time > interval '30 seconds' THEN
                      RAISE NOTICE
                          'wait for backend_xmin timed out after 30 s';
                      EXIT;
                  END IF;
                  PERFORM pg_sleep(0.02);
              END LOOP;
          END $WAIT$
          $$),
    (12, $$VACUUM unlogged_shrink$$),
    (13, $$INSERT INTO unlogged_shrink_diag (iter, pre_merge_segs)
          SELECT %1$s,
                 COALESCE(NULLIF(
                            array_to_string(
                              ARRAY(
                                SELECT regexp_replace(
                                         regexp_replace(line,
                                           'block=\d+, pages=\d+, size=[0-9.]+MB, ',
                                           '', 'g'),
                                         '\s+', ' ', 'g')
                                FROM unnest(string_to_array(
                                       bm25_summarize_index('unlogged_shrink_idx'),
                                       E'\n')) AS t(line)
                                WHERE line ~ '^\s*L\d+ Segment '
                                ORDER BY line
                              ),
                              E'\n'),
                            ''),
                          '<NO SEGMENTS>')$$),
    (14, $$SELECT bm25_force_merge('unlogged_shrink_idx')$$),
    (15, $$UPDATE unlogged_shrink_diag
          SET docs_persisted = COALESCE(
                  substring(s from 'docs_persisted: \d+'),
                  '<MISSING docs_persisted>'),
              post_merge_segs = COALESCE(NULLIF(
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
          FROM (SELECT bm25_summarize_index('unlogged_shrink_idx') AS s) sub
          WHERE iter = %1$s$$)
)
SELECT format(sql_template, n)
FROM generate_series(1, 8) n, steps
ORDER BY n, stepno
\gexec

RESET client_min_messages;
\o
\set ECHO queries

-- Distinct-value collapse.  On the happy path every iteration
-- produced `docs_persisted: 100` with one merged L1 segment
-- holding `docs=100`, so we get exactly one row and n_iter = 8.
-- A divergent docs_persisted or post_merge_segs would split the
-- group and signal the #397 regression has returned.
SELECT docs_persisted, post_merge_segs,
       count(*) AS n_iter
FROM unlogged_shrink_diag
GROUP BY docs_persisted, post_merge_segs
ORDER BY docs_persisted, post_merge_segs;

-- Outlier rows -- iterations whose (docs_persisted,
-- post_merge_segs) tuple does not match the most common.  Zero
-- rows on the happy path; on a flake this names the specific
-- iteration numbers and includes the post-VACUUM segment summary
-- so the regression .out diff carries enough evidence to confirm
-- whether the alive-bitset shrinkage step ran (segments should be
-- annotated `(alive=50, dead=50)` after VACUUM).
WITH most_common AS (
    SELECT docs_persisted, post_merge_segs
    FROM unlogged_shrink_diag
    GROUP BY docs_persisted, post_merge_segs
    ORDER BY count(*) DESC, docs_persisted, post_merge_segs
    LIMIT 1
)
SELECT d.iter, d.pre_merge_segs, d.docs_persisted, d.post_merge_segs
FROM unlogged_shrink_diag d, most_common m
WHERE (d.docs_persisted, d.post_merge_segs)
   IS DISTINCT FROM (m.docs_persisted, m.post_merge_segs)
ORDER BY d.iter;

-- Cleanup
DROP TABLE unlogged_shrink_diag;
DROP TABLE unlogged_shrink;
DROP TABLE unlogged_merge;
DROP TABLE unlogged_test;
DROP EXTENSION pg_textsearch CASCADE;