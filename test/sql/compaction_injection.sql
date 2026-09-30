\pset format unaligned
SET client_min_messages = warning;
CREATE EXTENSION pg_textsearch;
CREATE EXTENSION injection_points;
CREATE EXTENSION pg_textsearch_test;

SET pg_textsearch.segments_per_level = 64;
CREATE TABLE compaction_terminal (id integer, body text);
CREATE INDEX compaction_terminal_idx ON compaction_terminal
    USING bm25(body) WITH (text_config = 'simple', compaction = 'manual');
DO $$
BEGIN
    FOR batch IN 1..2 LOOP
        INSERT INTO compaction_terminal
        SELECT batch * 100 + n, 'common document ' || n
        FROM generate_series(1, 100) n;
        PERFORM bm25_spill_index('compaction_terminal_idx');
    END LOOP;
END
$$;

-- Tiny outputs no longer climb to L7 naturally. Construct the legacy layout.
SELECT pg_textsearch_test_move_level('compaction_terminal_idx', 0, 7);
SELECT pg_textsearch_test_attach_segment_limit(2);
SET pg_textsearch.segments_per_level = 2;
SELECT bm25_level_counts('compaction_terminal_idx') =
           ARRAY[0, 0, 0, 0, 0, 0, 0, 2]
       AND bm25_needs_compaction('compaction_terminal_idx')
       AS terminal_debt_constructed;
SELECT bm25_compact_step('compaction_terminal_idx')
       AS terminal_debt_reduced;
SELECT bm25_level_counts('compaction_terminal_idx') =
           ARRAY[0, 1, 0, 0, 0, 0, 0, 0]
       AND NOT bm25_needs_compaction('compaction_terminal_idx')
       AS terminal_output_placed_by_size;

-- Compatible segments on different levels are debt below both thresholds.
SELECT pg_textsearch_test_move_level('compaction_terminal_idx', 1, 7);
SET pg_textsearch.segments_per_level = 64;
INSERT INTO compaction_terminal
SELECT 1000 + n, 'common document ' || n FROM generate_series(1, 200) n;
SELECT bm25_spill_index('compaction_terminal_idx') > 0 AS lower_spilled;
SELECT bm25_level_counts('compaction_terminal_idx') =
           ARRAY[1, 0, 0, 0, 0, 0, 0, 1]
       AND bm25_needs_compaction('compaction_terminal_idx')
       AS cross_level_debt_constructed;
SELECT bm25_compact_step('compaction_terminal_idx')
       AS cross_level_pass_ran;
SELECT bm25_level_counts('compaction_terminal_idx') =
           ARRAY[0, 1, 0, 0, 0, 0, 0, 0]
       AND NOT bm25_compact_step('compaction_terminal_idx')
       AS cross_level_pass_converged;
SELECT count(*) = 400 AS cross_level_merge_preserves_matches
FROM (SELECT id FROM compaction_terminal
      ORDER BY body <@> to_bm25query('common', 'compaction_terminal_idx')
      LIMIT 1000) ranked;

-- Force merge also accepts terminal-level inputs.
SELECT pg_textsearch_test_move_level('compaction_terminal_idx', 1, 7);
SELECT bm25_force_merge('compaction_terminal_idx');
SELECT bm25_level_counts('compaction_terminal_idx') =
           ARRAY[1, 0, 0, 0, 0, 0, 0, 0]
       AS force_merge_reclassifies_terminal_segment;

SELECT injection_points_detach('pg-textsearch-segment-count-limit');

-- Background spills enqueue work without running the planner in the writer.
CREATE TABLE compaction_signal (id integer, body text);
CREATE INDEX compaction_signal_idx ON compaction_signal USING bm25(body)
    WITH (text_config = 'simple', compaction = 'manual');
INSERT INTO compaction_signal
SELECT n, 'common document ' || n FROM generate_series(1, 100) n;
SELECT bm25_spill_index('compaction_signal_idx') > 0 AS initial_spill;
BEGIN;
UPDATE pg_class SET reloptions = array_replace(
    reloptions, 'compaction=manual', 'compaction=background')
WHERE oid = 'compaction_signal_idx'::regclass;
INSERT INTO compaction_signal
SELECT n, 'common document ' || n FROM generate_series(101, 200) n;
SELECT injection_points_attach(
    'pg-textsearch-compaction-source-estimate', 'error');
SELECT bm25_spill_index('compaction_signal_idx') > 0
       AS background_spill_defers_planning;
ROLLBACK;
SELECT injection_points_detach('pg-textsearch-compaction-source-estimate');
DROP TABLE compaction_signal;
RESET pg_textsearch.segments_per_level;
DROP TABLE compaction_terminal CASCADE;
DROP EXTENSION pg_textsearch_test;
DROP EXTENSION injection_points;
DROP EXTENSION pg_textsearch CASCADE;
