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
SELECT bm25_compact_step('compaction_step_idx'::regclass)
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
RESET ROLE;
DROP OWNED BY compaction_user CASCADE;
DROP ROLE compaction_user;

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
DROP TABLE compaction_partitioned CASCADE;
DROP TABLE compaction_full CASCADE;
DROP TABLE compaction_step CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
