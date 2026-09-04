CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\pset format unaligned
SET client_min_messages = warning;
SET pg_textsearch.segments_per_level = 64;

CREATE TABLE compaction_step (id serial PRIMARY KEY, body text);
CREATE TABLE compaction_full (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_step_idx ON compaction_step
    USING bm25(body) WITH (text_config = 'english');
CREATE INDEX compaction_full_idx ON compaction_full
    USING bm25(body) WITH (text_config = 'english');
CREATE INDEX compaction_btree_idx ON compaction_step(id);
CREATE TABLE compaction_partitioned (id integer, body text)
    PARTITION BY RANGE (id);
CREATE TABLE compaction_partitioned_leaf
    PARTITION OF compaction_partitioned FOR VALUES FROM (0) TO (10);
CREATE INDEX compaction_partitioned_idx ON compaction_partitioned
    USING bm25(body) WITH (text_config = 'english');

-- New indexes expose all levels and have no compaction debt.
SELECT array_length(
           bm25_level_counts('compaction_step_idx'::regclass), 1) = 8
       AS has_all_levels;
SELECT bm25_needs_compaction('compaction_step_idx'::regclass)
       AS new_index_needs_compaction;

-- Non-bm25 relations are rejected.
SELECT bm25_level_counts('compaction_btree_idx'::regclass);
SELECT bm25_level_counts('compaction_step'::regclass);

-- Partitioned parents have no storage; their physical leaves remain usable.
SELECT bm25_level_counts('compaction_partitioned_idx'::regclass);
SELECT bm25_compact('compaction_partitioned_idx'::regclass);
SELECT bm25_compact_step('compaction_partitioned_idx'::regclass);
SELECT bm25_needs_compaction('compaction_partitioned_idx'::regclass);
SELECT array_length(bm25_level_counts(i.inhrelid), 1) = 8
       AS partition_leaf_counts_supported
FROM pg_inherits i
WHERE i.inhparent = 'compaction_partitioned_idx'::regclass;
SELECT bm25_compact(i.inhrelid)
FROM pg_inherits i
WHERE i.inhparent = 'compaction_partitioned_idx'::regclass;
SELECT NOT bm25_compact_step(i.inhrelid)
       AND NOT bm25_needs_compaction(i.inhrelid)
       AS partition_leaf_compaction_supported
FROM pg_inherits i
WHERE i.inhparent = 'compaction_partitioned_idx'::regclass;

-- Construct identical four-segment layouts without inline compaction.
DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..4 LOOP
        INSERT INTO compaction_step (body)
        SELECT format('step batch %s document %s filler', n, i)
        FROM generate_series(1, 20) i;
        INSERT INTO compaction_full (body)
        SELECT format('full batch %s document %s filler', n, i)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('compaction_step_idx');
        PERFORM bm25_spill_index('compaction_full_idx');
    END LOOP;
END
$$;

SET pg_textsearch.segments_per_level = 2;
SELECT bm25_level_counts('compaction_step_idx'::regclass) =
           ARRAY[4, 0, 0, 0, 0, 0, 0, 0]
       AND bm25_needs_compaction('compaction_step_idx'::regclass)
       AS step_starts_with_debt;

SELECT d.oid AS db_oid,
       c.oid AS index_oid,
       coalesce(nullif(c.reltablespace, 0), d.dattablespace) AS spc_oid,
       pg_relation_filenode(c.oid) AS relfilenumber,
       c.relowner AS owner_oid
FROM pg_class c
JOIN pg_database d ON d.datname = current_database()
WHERE c.oid = 'compaction_step_idx'::regclass
\gset target_

-- Stale physical identities are rejected without touching the live index.
SELECT NOT bm25_compact_step_if_current(
               :target_index_oid::oid, 0::oid, :target_spc_oid::oid,
               :target_relfilenumber::oid, :target_owner_oid::oid)
       AND NOT bm25_compact_step_if_current(
               :target_index_oid::oid, :target_db_oid::oid, 0::oid,
               :target_relfilenumber::oid, :target_owner_oid::oid)
       AND NOT bm25_compact_step_if_current(
               :target_index_oid::oid, :target_db_oid::oid,
               :target_spc_oid::oid, 0::oid, :target_owner_oid::oid)
       AND NOT bm25_compact_step_if_current(
               :target_index_oid::oid, :target_db_oid::oid,
               :target_spc_oid::oid, :target_relfilenumber::oid, 0::oid)
       AS stale_step_targets_rejected;

-- Permanent-index mutators reject read-only transactions.
BEGIN READ ONLY;
SELECT bm25_compact('compaction_step_idx'::regclass);
ROLLBACK;
BEGIN READ ONLY;
SELECT bm25_compact_step('compaction_step_idx'::regclass);
ROLLBACK;

-- In this layout one pass happens to merge exactly one batch.  A pass is
-- not limited to one batch in general; it may span several, and may pull
-- in further batches to unblock a destination level.
SELECT bm25_compact_step_if_current(
           :target_index_oid::oid, :target_db_oid::oid,
           :target_spc_oid::oid, :target_relfilenumber::oid,
           :target_owner_oid::oid)
       AS one_batch_ran;
SELECT bm25_level_counts('compaction_step_idx'::regclass) =
           ARRAY[2, 1, 0, 0, 0, 0, 0, 0]
       AND bm25_needs_compaction('compaction_step_idx'::regclass)
       AS one_batch_only;

-- Repeated steps converge, then report that no batch ran.
CREATE TEMP TABLE compaction_step_result (steps integer);
DO $$
DECLARE
    step_count integer := 0;
BEGIN
    WHILE bm25_needs_compaction('compaction_step_idx'::regclass) LOOP
        step_count := step_count + 1;
        IF step_count > 100 THEN
            RAISE EXCEPTION 'bm25_compact_step did not terminate';
        END IF;
        IF NOT bm25_compact_step('compaction_step_idx'::regclass) THEN
            RAISE EXCEPTION 'bm25_needs_compaction disagrees with step';
        END IF;
    END LOOP;
    INSERT INTO compaction_step_result VALUES (step_count);
END
$$;
SELECT steps = 2 AS step_split_cascade
FROM compaction_step_result;
SELECT NOT bm25_needs_compaction('compaction_step_idx'::regclass)
       AND NOT bm25_compact_step('compaction_step_idx'::regclass)
       AS step_converged;

-- Whole-index compaction reaches the same terminal layout.
SELECT bm25_compact('compaction_full_idx'::regclass);
SELECT NOT bm25_needs_compaction('compaction_full_idx'::regclass)
       AND bm25_level_counts('compaction_step_idx'::regclass) =
           bm25_level_counts('compaction_full_idx'::regclass)
       AS full_compaction_converged;
SELECT count(*) = 80 AS step_preserves_documents
FROM (
    SELECT 1
    FROM compaction_step
    ORDER BY body <@> to_bm25query('filler', 'compaction_step_idx')
) ranked;
SELECT count(*) = 80 AS full_preserves_documents
FROM (
    SELECT 1
    FROM compaction_full
    ORDER BY body <@> to_bm25query('filler', 'compaction_full_idx')
) ranked;

-- Background workers validate mode and physical identity without writing.
CREATE TABLE compaction_background_target (id integer, body text);
CREATE INDEX compaction_background_target_idx
    ON compaction_background_target USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
SELECT d.oid AS db_oid,
       c.oid AS index_oid,
       coalesce(nullif(c.reltablespace, 0), d.dattablespace) AS spc_oid,
       pg_relation_filenode(c.oid) AS relfilenumber,
       c.relowner AS owner_oid
FROM pg_class c
JOIN pg_database d ON d.datname = current_database()
WHERE c.oid = 'compaction_background_target_idx'::regclass
\gset background_
SELECT bm25_background_target_is_current(
           :background_index_oid::oid, :background_db_oid::oid,
           :background_spc_oid::oid, :background_relfilenumber::oid,
           :background_owner_oid::oid)
       AS background_target_is_current;
ALTER INDEX compaction_background_target_idx
    SET (compaction = 'manual');
SELECT NOT bm25_background_target_is_current(
               :background_index_oid::oid, :background_db_oid::oid,
               :background_spc_oid::oid, :background_relfilenumber::oid,
               :background_owner_oid::oid)
       AS manual_target_is_not_current;

-- Inspection is public, but only the index owner may mutate it.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'compaction_user') THEN
        EXECUTE 'REASSIGN OWNED BY compaction_user TO CURRENT_USER';
        EXECUTE 'DROP OWNED BY compaction_user CASCADE';
        DROP ROLE compaction_user;
    END IF;
END
$$;
CREATE ROLE compaction_user LOGIN;
GRANT USAGE ON SCHEMA public TO compaction_user;
SET ROLE compaction_user;
SELECT array_length(
           bm25_level_counts('compaction_step_idx'::regclass), 1) = 8
       AS nonowner_can_inspect;
SELECT bm25_needs_compaction('compaction_step_idx'::regclass)
       AS nonowner_can_check;
SELECT bm25_compact('compaction_step_idx'::regclass);
SELECT bm25_compact_step('compaction_step_idx'::regclass);
SELECT bm25_compact_step_if_current(
           :target_index_oid::oid, :target_db_oid::oid,
           :target_spc_oid::oid, :target_relfilenumber::oid,
           :target_owner_oid::oid);
SELECT bm25_background_target_is_current(
           :background_index_oid::oid, :background_db_oid::oid,
           :background_spc_oid::oid, :background_relfilenumber::oid,
           :background_owner_oid::oid);
RESET ROLE;
DROP OWNED BY compaction_user CASCADE;
DROP ROLE compaction_user;
DROP TABLE compaction_background_target CASCADE;

-- Local temporary indexes remain mutable in read-only transactions.
SET pg_textsearch.segments_per_level = 64;
CREATE TEMP TABLE compaction_temp_step (id serial, body text);
CREATE TEMP TABLE compaction_temp_full (id serial, body text);
CREATE INDEX compaction_temp_step_idx ON compaction_temp_step
    USING bm25(body) WITH (text_config = 'english');
CREATE INDEX compaction_temp_full_idx ON compaction_temp_full
    USING bm25(body) WITH (text_config = 'english');
DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO compaction_temp_step (body)
        VALUES (format('temporary step document %s filler', n));
        INSERT INTO compaction_temp_full (body)
        VALUES (format('temporary full document %s filler', n));
        PERFORM bm25_spill_index('compaction_temp_step_idx');
        PERFORM bm25_spill_index('compaction_temp_full_idx');
    END LOOP;
END
$$;
SET pg_textsearch.segments_per_level = 2;
BEGIN READ ONLY;
SELECT bm25_compact_step('compaction_temp_step_idx'::regclass)
       AS temp_step_ran;
SELECT bm25_compact('compaction_temp_full_idx'::regclass);
COMMIT;
SELECT NOT bm25_needs_compaction('compaction_temp_step_idx'::regclass)
       AND NOT bm25_needs_compaction(
           'compaction_temp_full_idx'::regclass)
       AS temp_indexes_converged;

-- A published pass is a physical change, so ROLLBACK does not undo it.
-- A caller must not treat a step as transactional work.
CREATE TABLE compaction_rollback (id serial, body text);
CREATE INDEX compaction_rollback_idx ON compaction_rollback
    USING bm25(body) WITH (text_config = 'english');
SET pg_textsearch.segments_per_level = 64;
DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..4 LOOP
        INSERT INTO compaction_rollback (body)
        VALUES (format('rollback document %s filler', n));
        PERFORM bm25_spill_index('compaction_rollback_idx');
    END LOOP;
END
$$;
SET pg_textsearch.segments_per_level = 2;
CREATE TEMP TABLE compaction_rollback_before AS
SELECT bm25_level_counts('compaction_rollback_idx'::regclass) AS counts;
BEGIN;
SELECT bm25_compact_step('compaction_rollback_idx'::regclass)
       AS rollback_step_ran;
ROLLBACK;
SELECT bm25_level_counts('compaction_rollback_idx'::regclass)
           <> before.counts
       AS rollback_does_not_undo_a_published_pass
FROM compaction_rollback_before before;
DROP TABLE compaction_rollback CASCADE;

-- A level can sit at the segment threshold with nothing to compact:
-- every candidate group already exceeds max_segment_size, and an
-- over-budget segment is an uncombinable singleton.  This is not
-- compaction debt -- no pass would reduce it -- but
-- bm25_needs_compaction() is a count-only signal and cannot tell the
-- two apart, so it reports the full level that bm25_compact_step()
-- correctly declines to act on.  A scheduler that retried on a false
-- return would spin here.
CREATE TABLE compaction_unreducible (id bigint PRIMARY KEY, body text);
CREATE INDEX compaction_unreducible_idx ON compaction_unreducible
    USING bm25(body) WITH (text_config = 'simple');
SET pg_textsearch.max_segment_size = '1MB';
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 0;
SET pg_textsearch.segments_per_level = 2;
DO $$
DECLARE
    batch integer;
BEGIN
    FOR batch IN 0..1 LOOP
        INSERT INTO compaction_unreducible
        SELECT batch * 2000 + gs,
               'common ' || repeat(md5((batch * 2000 + gs)::text), 32)
        FROM generate_series(1, 2000) gs;
        PERFORM bm25_spill_index('compaction_unreducible_idx');
    END LOOP;
END
$$;
SELECT bm25_level_counts('compaction_unreducible_idx'::regclass) =
           ARRAY[2, 0, 0, 0, 0, 0, 0, 0]
       AS unreducible_level_is_at_threshold;
SELECT bm25_needs_compaction('compaction_unreducible_idx'::regclass)
       AS unreducible_reports_full_level;
SELECT bm25_compact_step('compaction_unreducible_idx'::regclass)
       AS unreducible_step_declines;
SELECT bm25_compact('compaction_unreducible_idx'::regclass);
SELECT bm25_level_counts('compaction_unreducible_idx'::regclass) =
           ARRAY[2, 0, 0, 0, 0, 0, 0, 0]
       AS unreducible_full_is_a_noop;
SELECT count(*) = 4000 AS unreducible_preserves_documents
FROM (
    SELECT 1
    FROM compaction_unreducible
    ORDER BY body <@> to_bm25query('common', 'compaction_unreducible_idx')
    LIMIT 5000
) ranked;
RESET pg_textsearch.max_segment_size;
RESET pg_textsearch.memtable_pages_threshold;
RESET pg_textsearch.bulk_load_threshold;
DROP TABLE compaction_unreducible CASCADE;

-- A pass must not copy segments it cannot combine.  Spills prepend, so
-- a level reads newest-first and its oldest, largest runs sit at the
-- tail.  Here two fresh small segments head a 2MB run that is already
-- over max_segment_size: the pair merges, and the run is handed back
-- instead of being rewritten to say the same thing in a new place.
--
-- The handback is visible as a level count: a retained segment keeps
-- its level, while a rewritten one is promoted.  Without the trim this
-- ends at [0, 2, ...] with the run copied to a fresh block, and the
-- index grows by the full size of the run rather than by the pair.
CREATE TABLE compaction_uncombinable_tail (id bigint PRIMARY KEY, body text);
CREATE INDEX compaction_uncombinable_tail_idx ON compaction_uncombinable_tail
    USING bm25(body) WITH (text_config = 'simple');
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 0;
SET pg_textsearch.max_segment_size = '1MB';
-- Build the layout with a fanout no spill can reach, so that the only
-- compaction in this fixture is the one under test.
SET pg_textsearch.segments_per_level = 64;
INSERT INTO compaction_uncombinable_tail
SELECT gs, 'common ' || repeat(md5(gs::text), 32)
FROM generate_series(1, 2000) gs;
SELECT bm25_spill_index('compaction_uncombinable_tail_idx') > 0
       AS tail_run_spilled;
INSERT INTO compaction_uncombinable_tail
SELECT 100000 + gs, 'common small ' || gs FROM generate_series(1, 20) gs;
SELECT bm25_spill_index('compaction_uncombinable_tail_idx') > 0
       AS head_pair_first_spilled;
INSERT INTO compaction_uncombinable_tail
SELECT 200000 + gs, 'common small ' || gs FROM generate_series(1, 20) gs;
SELECT bm25_spill_index('compaction_uncombinable_tail_idx') > 0
       AS head_pair_second_spilled;
SET pg_textsearch.segments_per_level = 3;
SELECT bm25_level_counts('compaction_uncombinable_tail_idx'::regclass) =
           ARRAY[3, 0, 0, 0, 0, 0, 0, 0]
       AS tail_level_is_at_threshold;
CREATE TEMP TABLE compaction_tail_before AS
SELECT pg_relation_size('compaction_uncombinable_tail_idx') AS bytes;
SELECT bm25_compact('compaction_uncombinable_tail_idx'::regclass);
SELECT bm25_level_counts('compaction_uncombinable_tail_idx'::regclass) =
           ARRAY[1, 1, 0, 0, 0, 0, 0, 0]
       AS tail_run_retained_at_its_level;
SELECT pg_relation_size('compaction_uncombinable_tail_idx') - before.bytes
           < 1024 * 1024
       AS tail_run_was_not_copied
FROM compaction_tail_before before;
SELECT count(*) = 2040 AS tail_preserves_documents
FROM (
    SELECT 1
    FROM compaction_uncombinable_tail
    ORDER BY body <@> to_bm25query(
        'common', 'compaction_uncombinable_tail_idx')
    LIMIT 5000
) ranked;
RESET pg_textsearch.max_segment_size;
RESET pg_textsearch.memtable_pages_threshold;
RESET pg_textsearch.bulk_load_threshold;
DROP TABLE compaction_uncombinable_tail CASCADE;

-- The top level is a terminal bucket, not a wall.  With a per-level
-- count limit of 2, driving 384 segments up the ladder once failed
-- closed with "segment count limit reached at level 7", because the
-- top level was never a compaction candidate and so could never make
-- room for a promotion out of L6.  It now compacts into itself, so
-- the ladder drains instead of jamming.  (Reclaim of already-parked
-- pages may also run; only the published level counts are asserted.)
CREATE TABLE compaction_terminal (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_terminal_idx ON compaction_terminal
    USING bm25(body) WITH (text_config = 'english');
SET pg_textsearch.debug_segment_count_limit = 2;
DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..384 LOOP
        PERFORM set_config(
            'pg_textsearch.segments_per_level', '64', true);
        INSERT INTO compaction_terminal (body)
        VALUES (format('terminal segment document %s filler', n));
        PERFORM bm25_spill_index('compaction_terminal_idx');
        PERFORM set_config(
            'pg_textsearch.segments_per_level', '2', true);

        -- The documented driver shape: stop on the step's own report
        -- rather than on bm25_needs_compaction, which stays true on
        -- debt no pass can reduce.
        WHILE bm25_needs_compaction(
                  'compaction_terminal_idx'::regclass) LOOP
            EXIT WHEN NOT bm25_compact_step(
                              'compaction_terminal_idx'::regclass);
        END LOOP;
    END LOOP;
END
$$;
SET pg_textsearch.segments_per_level = 2;
SELECT bm25_level_counts('compaction_terminal_idx'::regclass) =
           ARRAY[0, 0, 0, 0, 0, 0, 0, 1]
       AND NOT bm25_needs_compaction(
                   'compaction_terminal_idx'::regclass)
       AS top_level_drains;
-- The ladder has settled: both entry points report no work rather
-- than raising a capacity error.
SELECT bm25_compact_step('compaction_terminal_idx'::regclass);
SELECT bm25_compact('compaction_terminal_idx'::regclass);
SELECT bm25_level_counts('compaction_terminal_idx'::regclass) =
           ARRAY[0, 0, 0, 0, 0, 0, 0, 1]
       AS terminal_is_settled;

-- Lower-level debt compacts normally against a populated top level.
SET pg_textsearch.segments_per_level = 64;
DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO compaction_terminal (body)
        VALUES (format('mixed lower segment document %s filler', n));
        PERFORM bm25_spill_index('compaction_terminal_idx');
    END LOOP;
END
$$;
SET pg_textsearch.segments_per_level = 2;
SELECT bm25_level_counts('compaction_terminal_idx'::regclass) =
           ARRAY[2, 0, 0, 0, 0, 0, 0, 1]
       AS mixed_lower_debt_starts;
SELECT bm25_compact('compaction_terminal_idx'::regclass);
SELECT bm25_level_counts('compaction_terminal_idx'::regclass) =
           ARRAY[0, 1, 0, 0, 0, 0, 0, 1]
       AS mixed_full_compacts_lower_debt;
SELECT count(*) = 386 AS mixed_compaction_preserves_documents
FROM (
    SELECT 1
    FROM compaction_terminal
    ORDER BY body <@> to_bm25query('filler', 'compaction_terminal_idx')
) ranked;
-- With every level under threshold a step is a no-op, not an error.
CREATE TEMP TABLE compaction_mixed_after AS
SELECT bm25_level_counts('compaction_terminal_idx'::regclass) AS counts;
SELECT bm25_compact_step('compaction_terminal_idx'::regclass);
SELECT bm25_level_counts('compaction_terminal_idx'::regclass) =
           after.counts
       AS mixed_step_is_a_noop
FROM compaction_mixed_after after;
SELECT count(*) = 386 AS terminal_preserves_documents
FROM (
    SELECT 1
    FROM compaction_terminal
    ORDER BY body <@> to_bm25query('filler', 'compaction_terminal_idx')
) ranked;

RESET pg_textsearch.debug_segment_count_limit;
RESET pg_textsearch.segments_per_level;
DROP TABLE compaction_terminal CASCADE;

-- The sweep runs one pass for each non-temporary background index.
SET pg_textsearch.segments_per_level = 64;
CREATE TABLE compaction_sweep_background (id serial, body text);
CREATE UNLOGGED TABLE compaction_sweep_unlogged (id serial, body text);
CREATE TABLE compaction_sweep_inline (id serial, body text);
CREATE TABLE compaction_sweep_manual (id serial, body text);
CREATE INDEX compaction_sweep_background_idx
    ON compaction_sweep_background USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE INDEX compaction_sweep_unlogged_idx
    ON compaction_sweep_unlogged USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE INDEX compaction_sweep_inline_idx
    ON compaction_sweep_inline USING bm25(body)
    WITH (text_config = 'english', compaction = 'inline');
CREATE INDEX compaction_sweep_manual_idx
    ON compaction_sweep_manual USING bm25(body)
    WITH (text_config = 'english', compaction = 'manual');
DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO compaction_sweep_background (body)
        VALUES (format('background sweep document %s', n));
        INSERT INTO compaction_sweep_unlogged (body)
        VALUES (format('unlogged sweep document %s', n));
        INSERT INTO compaction_sweep_inline (body)
        VALUES (format('inline sweep document %s', n));
        INSERT INTO compaction_sweep_manual (body)
        VALUES (format('manual sweep document %s', n));
        PERFORM bm25_spill_index('compaction_sweep_background_idx');
        PERFORM bm25_spill_index('compaction_sweep_unlogged_idx');
        PERFORM bm25_spill_index('compaction_sweep_inline_idx');
        PERFORM bm25_spill_index('compaction_sweep_manual_idx');
    END LOOP;
END
$$;
SET pg_textsearch.segments_per_level = 2;
SELECT bm25_compact_pending() AS background_sweep_passes;
SELECT bm25_level_counts(
           'compaction_sweep_background_idx'::regclass) =
           ARRAY[0, 1, 0, 0, 0, 0, 0, 0]
       AS background_sweep_ran_one_pass;
SELECT bm25_level_counts(
           'compaction_sweep_unlogged_idx'::regclass) =
           ARRAY[0, 1, 0, 0, 0, 0, 0, 0]
       AS unlogged_background_is_eligible;
SELECT bm25_level_counts('compaction_sweep_inline_idx'::regclass) =
           ARRAY[2, 0, 0, 0, 0, 0, 0, 0]
       AND bm25_level_counts('compaction_sweep_manual_idx'::regclass) =
           ARRAY[2, 0, 0, 0, 0, 0, 0, 0]
       AS non_background_indexes_untouched;
SELECT bm25_compact_pending() AS no_reducible_passes;
DROP TABLE compaction_sweep_background CASCADE;
DROP TABLE compaction_sweep_unlogged CASCADE;
DROP TABLE compaction_sweep_inline CASCADE;
DROP TABLE compaction_sweep_manual CASCADE;

-- Every catalog predicate is required before an index reaches the step.
CREATE TABLE compaction_sweep_catalog (id integer, body text);
CREATE INDEX compaction_sweep_valid_idx
    ON compaction_sweep_catalog USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE INDEX compaction_sweep_invalid_idx
    ON compaction_sweep_catalog USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE INDEX compaction_sweep_not_ready_idx
    ON compaction_sweep_catalog USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE INDEX compaction_sweep_not_live_idx
    ON compaction_sweep_catalog USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE INDEX compaction_sweep_btree_idx ON compaction_sweep_catalog(id);
CREATE TABLE compaction_sweep_partitioned (id integer, body text)
    PARTITION BY RANGE (id);
CREATE INDEX compaction_sweep_partitioned_idx
    ON compaction_sweep_partitioned USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE TEMP TABLE compaction_sweep_temp (id integer, body text);
CREATE INDEX compaction_sweep_temp_idx
    ON compaction_sweep_temp USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
UPDATE pg_catalog.pg_class
SET reloptions = ARRAY['compaction=background']
WHERE oid = 'compaction_sweep_btree_idx'::regclass;
UPDATE pg_catalog.pg_index
SET indisvalid = false
WHERE indexrelid = 'compaction_sweep_invalid_idx'::regclass;
UPDATE pg_catalog.pg_index
SET indisready = false
WHERE indexrelid = 'compaction_sweep_not_ready_idx'::regclass;
UPDATE pg_catalog.pg_index
SET indislive = false
WHERE indexrelid = 'compaction_sweep_not_live_idx'::regclass;
CREATE TEMP TABLE compaction_sweep_calls (index_oid oid);
ALTER FUNCTION bm25_compact_step(regclass)
    RENAME TO bm25_compact_step_real;
CREATE FUNCTION bm25_compact_step(idx regclass)
RETURNS boolean
LANGUAGE plpgsql VOLATILE STRICT
AS $$
BEGIN
    INSERT INTO pg_temp.compaction_sweep_calls VALUES (idx::oid);
    RETURN true;
END
$$;
DISCARD PLANS;
SELECT bm25_compact_pending() AS catalog_eligible_count;
SELECT count(*) = 1
       AND bool_and(
               index_oid = 'compaction_sweep_valid_idx'::regclass::oid)
       AS catalog_exclusions_hold
FROM compaction_sweep_calls;
DROP FUNCTION bm25_compact_step(regclass);
ALTER FUNCTION bm25_compact_step_real(regclass)
    RENAME TO bm25_compact_step;
DISCARD PLANS;
UPDATE pg_catalog.pg_class
SET reloptions = NULL
WHERE oid = 'compaction_sweep_btree_idx'::regclass;
UPDATE pg_catalog.pg_index
SET indisvalid = true
WHERE indexrelid = 'compaction_sweep_invalid_idx'::regclass;
UPDATE pg_catalog.pg_index
SET indisready = true
WHERE indexrelid = 'compaction_sweep_not_ready_idx'::regclass;
UPDATE pg_catalog.pg_index
SET indislive = true
WHERE indexrelid = 'compaction_sweep_not_live_idx'::regclass;
DROP TABLE compaction_sweep_temp;
DROP TABLE compaction_sweep_partitioned;
DROP TABLE compaction_sweep_catalog CASCADE;

-- An ownership error warns and does not prevent a later eligible index.
CREATE ROLE compaction_sweep_owner;
CREATE ROLE compaction_sweep_runner;
GRANT USAGE, CREATE ON SCHEMA public TO compaction_sweep_owner;
GRANT USAGE, CREATE ON SCHEMA public TO compaction_sweep_runner;
SET pg_textsearch.segments_per_level = 64;
SET ROLE compaction_sweep_owner;
CREATE TABLE compaction_sweep_denied (id serial, body text);
CREATE INDEX deny_i
    ON compaction_sweep_denied USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO compaction_sweep_denied (body)
        VALUES (format('denied sweep document %s', n));
        PERFORM bm25_spill_index('deny_i');
    END LOOP;
END
$$;
RESET ROLE;
SET ROLE compaction_sweep_runner;
CREATE TABLE compaction_sweep_allowed (id serial, body text);
CREATE INDEX compaction_sweep_allowed_idx
    ON compaction_sweep_allowed USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO compaction_sweep_allowed (body)
        VALUES (format('allowed sweep document %s', n));
        PERFORM bm25_spill_index('compaction_sweep_allowed_idx');
    END LOOP;
END
$$;
RESET ROLE;
SET pg_textsearch.segments_per_level = 2;
SET ROLE compaction_sweep_runner;
SELECT bm25_compact_pending() AS sweep_continues_after_error;
RESET ROLE;
SELECT bm25_level_counts('deny_i'::regclass) =
           ARRAY[2, 0, 0, 0, 0, 0, 0, 0]
       AND bm25_level_counts('compaction_sweep_allowed_idx'::regclass) =
           ARRAY[0, 1, 0, 0, 0, 0, 0, 0]
       AS sweep_error_isolated;
DROP OWNED BY compaction_sweep_owner CASCADE;
DROP OWNED BY compaction_sweep_runner CASCADE;
DROP ROLE compaction_sweep_owner;
DROP ROLE compaction_sweep_runner;

-- Cancellation and shutdown errors always escape the per-index handler.
CREATE TABLE compaction_sweep_signal_table (id integer, body text);
CREATE INDEX compaction_sweep_signal_idx
    ON compaction_sweep_signal_table USING bm25(body)
    WITH (text_config = 'english', compaction = 'background');
CREATE TEMP TABLE compaction_sweep_signal (error_code text);
INSERT INTO compaction_sweep_signal VALUES ('57014');
CREATE TEMP TABLE compaction_sweep_rethrows (error_code text);
ALTER FUNCTION bm25_compact_step(regclass)
    RENAME TO bm25_compact_step_real;
CREATE FUNCTION bm25_compact_step(idx regclass)
RETURNS boolean
LANGUAGE plpgsql VOLATILE STRICT
AS $$
DECLARE
    code text;
BEGIN
    SELECT error_code INTO STRICT code
    FROM pg_temp.compaction_sweep_signal;
    RAISE EXCEPTION USING
        ERRCODE = code,
        MESSAGE = format('sweep test error %s', code);
END
$$;
DISCARD PLANS;
DO $$
DECLARE
    expected_state text;
BEGIN
    FOREACH expected_state IN ARRAY ARRAY['57014', '57P01', '57P02']
    LOOP
        UPDATE compaction_sweep_signal SET error_code = expected_state;
        BEGIN
            PERFORM bm25_compact_pending();
            RAISE EXCEPTION 'SQLSTATE % was not rethrown', expected_state;
        EXCEPTION
            WHEN SQLSTATE '57014' OR SQLSTATE '57P01' OR SQLSTATE '57P02'
            THEN
                IF SQLSTATE <> expected_state THEN
                    RAISE EXCEPTION 'expected SQLSTATE %, got %',
                        expected_state, SQLSTATE;
                END IF;
                INSERT INTO compaction_sweep_rethrows VALUES (SQLSTATE);
        END;
    END LOOP;
END
$$;
SELECT array_agg(error_code ORDER BY error_code) =
           ARRAY['57014', '57P01', '57P02']
       AS sweep_rethrows_cancellation_and_shutdown
FROM compaction_sweep_rethrows;
DROP FUNCTION bm25_compact_step(regclass);
ALTER FUNCTION bm25_compact_step_real(regclass)
    RENAME TO bm25_compact_step;
DISCARD PLANS;
DROP TABLE compaction_sweep_signal_table;

RESET pg_textsearch.segments_per_level;
DROP TABLE compaction_partitioned CASCADE;
DROP TABLE compaction_full CASCADE;
DROP TABLE compaction_step CASCADE;
SELECT NOT bm25_compact_step_if_current(
               :target_index_oid::oid, :target_db_oid::oid,
               :target_spc_oid::oid, :target_relfilenumber::oid,
               :target_owner_oid::oid)
       AS dropped_step_target_rejected;
DROP EXTENSION pg_textsearch CASCADE;
