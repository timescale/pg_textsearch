CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\pset format unaligned
SET client_min_messages = warning;
SET pg_textsearch.segments_per_level = 8;

CREATE TABLE compaction_test (
    id serial PRIMARY KEY,
    body text
);

CREATE INDEX compaction_test_idx ON compaction_test
    USING bm25(body) WITH (text_config = 'english');

CREATE INDEX compaction_test_btree_idx ON compaction_test(id);

-- Candidate enumeration includes bm25 indexes and excludes btree indexes.
SELECT EXISTS (
    SELECT 1
    FROM bm25_indexes_needing_compaction() AS t(idx)
    WHERE t.idx = 'compaction_test_idx'::regclass
) AS compaction_enum_includes_bm25;
SELECT NOT EXISTS (
    SELECT 1
    FROM bm25_indexes_needing_compaction() AS t(idx)
    WHERE t.idx = 'compaction_test_btree_idx'::regclass
) AS compaction_enum_excludes_btree;

CREATE TABLE compaction_partitioned (
    id integer,
    body text
) PARTITION BY RANGE (id);
CREATE TABLE compaction_partitioned_p0
    PARTITION OF compaction_partitioned FOR VALUES FROM (0) TO (10);
CREATE INDEX compaction_partitioned_idx ON compaction_partitioned
    USING bm25(body) WITH (text_config = 'english');

SELECT NOT EXISTS (
    SELECT 1
    FROM bm25_indexes_needing_compaction() AS t(idx)
         JOIN pg_class c ON c.oid = t.idx
    WHERE c.relkind = 'I'
) AS compaction_enum_excludes_partitioned_parents;
SELECT EXISTS (
    SELECT 1
    FROM bm25_indexes_needing_compaction() AS t(idx)
         JOIN pg_index i ON i.indexrelid = t.idx
    WHERE i.indrelid = 'compaction_partitioned_p0'::regclass
) AS compaction_enum_includes_partition_leaf;

-- A new bm25 index reports one count per LSM level.
SELECT array_length(bm25_level_counts('compaction_test_idx'::regclass), 1)
       AS level_count_length;

-- Manual spills create L0 segments, and the observable L0 count grows.
INSERT INTO compaction_test (body)
SELECT 'alpha document ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_test_idx') IS NOT NULL AS spill1;
SELECT (bm25_level_counts('compaction_test_idx'::regclass))[1]
       AS l0_after_spill1;

INSERT INTO compaction_test (body)
SELECT 'beta document ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_test_idx') IS NOT NULL AS spill2;
SELECT (bm25_level_counts('compaction_test_idx'::regclass))[1]
       AS l0_after_spill2;

-- Non-bm25 relations are rejected with a clear object-type error.
SELECT bm25_level_counts('compaction_test_btree_idx'::regclass);
SELECT bm25_level_counts('compaction_test'::regclass);

-- The compaction_mode GUC accepts the documented values only.
SET pg_textsearch.compaction_mode = 'inline';
SELECT current_setting('pg_textsearch.compaction_mode') = 'inline'
       AS compaction_mode_accepts_inline;
SET pg_textsearch.compaction_mode = 'background';
SELECT current_setting('pg_textsearch.compaction_mode') = 'background'
       AS compaction_mode_accepts_background;
SET pg_textsearch.compaction_mode = 'off';
SELECT current_setting('pg_textsearch.compaction_mode') = 'off'
       AS compaction_mode_accepts_off;
SET pg_textsearch.compaction_mode = 'bogus';
SET pg_textsearch.compaction_mode = 'inline';

-- Auto-compaction mode controls the spill-time compaction call site.
SET pg_textsearch.segments_per_level = 2;
SET pg_textsearch.compaction_mode = 'off';
CREATE TABLE compaction_mode_off (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_mode_off_idx ON compaction_mode_off
    USING bm25(body) WITH (text_config = 'english');

INSERT INTO compaction_mode_off (body)
SELECT 'mode off alpha ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_mode_off_idx') IS NOT NULL
       AS compaction_mode_off_spill1;

INSERT INTO compaction_mode_off (body)
SELECT 'mode off beta ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_mode_off_idx') IS NOT NULL
       AS compaction_mode_off_spill2;
SELECT (bm25_level_counts('compaction_mode_off_idx'::regclass))[1] >= 2
       AS compaction_mode_off_keeps_l0_segments;
SELECT bm25_needs_compaction('compaction_mode_off_idx'::regclass)
       AS compaction_mode_off_needs_compaction;

SET pg_textsearch.compaction_mode = 'inline';
INSERT INTO compaction_mode_off (body)
SELECT 'mode inline gamma ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_mode_off_idx') IS NOT NULL
       AS compaction_mode_inline_spill;
SELECT (bm25_level_counts('compaction_mode_off_idx'::regclass))[1] < 2
       AS compaction_mode_inline_collapses_l0;
SELECT NOT bm25_needs_compaction('compaction_mode_off_idx'::regclass)
       AS compaction_mode_inline_clears_pending;

-- Task 5 adds the background consumer; for now background only records.
SET pg_textsearch.compaction_mode = 'background';
CREATE TABLE compaction_mode_bg (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_mode_bg_idx ON compaction_mode_bg
    USING bm25(body) WITH (text_config = 'english');
BEGIN;
INSERT INTO compaction_mode_bg (body)
SELECT 'mode background alpha ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_mode_bg_idx') IS NOT NULL
       AS compaction_mode_bg_spill1;
INSERT INTO compaction_mode_bg (body)
SELECT 'mode background beta ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_mode_bg_idx') IS NOT NULL
       AS compaction_mode_bg_spill2;
COMMIT;
SELECT count(*) = 40 AS compaction_mode_bg_committed
FROM compaction_mode_bg;
SELECT (bm25_level_counts('compaction_mode_bg_idx'::regclass))[1] >= 2
       AS compaction_mode_bg_keeps_l0_segments;
SELECT bm25_needs_compaction('compaction_mode_bg_idx'::regclass)
       AS compaction_mode_bg_needs_compaction;
RESET pg_textsearch.compaction_mode;
DROP TABLE compaction_mode_bg CASCADE;
DROP TABLE compaction_mode_off CASCADE;

-- bm25_compact reduces the occupied lower level and promotes upward.
SET pg_textsearch.segments_per_level = 64;
CREATE TABLE compaction_manual (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_manual_idx ON compaction_manual
    USING bm25(body) WITH (text_config = 'english');

INSERT INTO compaction_manual (body)
SELECT 'compact alpha ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_manual_idx') IS NOT NULL
       AS manual_spill1;

INSERT INTO compaction_manual (body)
SELECT 'compact beta ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_manual_idx') IS NOT NULL
       AS manual_spill2;

INSERT INTO compaction_manual (body)
SELECT 'compact gamma ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_manual_idx') IS NOT NULL
       AS manual_spill3;

SET pg_textsearch.segments_per_level = 2;
SELECT (bm25_level_counts('compaction_manual_idx'::regclass))[1] >= 2
       AS manual_l0_over_threshold;
SELECT bm25_compact('compaction_manual_idx'::regclass);
SELECT (bm25_level_counts('compaction_manual_idx'::regclass))[1] < 2
       AS manual_l0_below_threshold;
SELECT (bm25_level_counts('compaction_manual_idx'::regclass))[2] > 0
       AS manual_l1_promoted;

-- Stepped compaction terminates and matches whole-cascade compaction.
SET pg_textsearch.segments_per_level = 64;
CREATE TABLE compaction_step_a (id serial PRIMARY KEY, body text);
CREATE TABLE compaction_step_b (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_step_a_idx ON compaction_step_a
    USING bm25(body) WITH (text_config = 'english');
CREATE INDEX compaction_step_b_idx ON compaction_step_b
    USING bm25(body) WITH (text_config = 'english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..4 LOOP
        INSERT INTO compaction_step_a (body)
        SELECT format('step batch %s doc %s filler filler filler', n, i)
        FROM generate_series(1, 20) i;
        INSERT INTO compaction_step_b (body)
        SELECT format('step batch %s doc %s filler filler filler', n, i)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('compaction_step_a_idx');
        PERFORM bm25_spill_index('compaction_step_b_idx');
    END LOOP;
END $$;

SET pg_textsearch.segments_per_level = 2;
SELECT bm25_compact_step('compaction_step_b_idx'::regclass)
       AS full_stage_step1;
SELECT bm25_compact_step('compaction_step_b_idx'::regclass)
       AS full_stage_step2;
SELECT (bm25_level_counts('compaction_step_b_idx'::regclass))[1] < 2
       AND
       (bm25_level_counts('compaction_step_b_idx'::regclass))[2] >= 2
       AS full_stage_higher_only;
SELECT bm25_compact('compaction_step_b_idx'::regclass);
SELECT NOT bm25_needs_compaction('compaction_step_b_idx'::regclass)
       AS full_compact_clears_higher_level;

BEGIN READ ONLY;
SELECT bm25_compact('compaction_step_a_idx'::regclass);
ROLLBACK;
BEGIN READ ONLY;
SELECT bm25_compact_step('compaction_step_a_idx'::regclass);
ROLLBACK;

SELECT bm25_needs_compaction('compaction_step_a_idx'::regclass)
       AS step_needs_before;
SELECT bm25_compact_step('compaction_step_a_idx'::regclass)
       AS step_first_returns_true;
CREATE TEMP TABLE compaction_step_result (steps integer);
DO $$
DECLARE
    step_count integer := 0;
BEGIN
    WHILE bm25_needs_compaction('compaction_step_a_idx'::regclass) LOOP
        step_count := step_count + 1;
        IF step_count > 100 THEN
            RAISE EXCEPTION 'bm25_compact_step did not terminate';
        END IF;
        IF NOT bm25_compact_step('compaction_step_a_idx'::regclass) THEN
            RAISE EXCEPTION 'bm25_needs_compaction disagrees with step';
        END IF;
    END LOOP;
    INSERT INTO compaction_step_result VALUES (step_count);
END $$;
SELECT steps AS compact_step_count FROM compaction_step_result;
SELECT steps >= 2 AS compact_step_split_cascade
FROM compaction_step_result;
SELECT NOT bm25_needs_compaction('compaction_step_a_idx'::regclass)
       AS step_needs_after;
SELECT NOT bm25_compact_step('compaction_step_a_idx'::regclass)
       AS step_final_returns_false;
SELECT bm25_compact('compaction_step_b_idx'::regclass);
SELECT bm25_level_counts('compaction_step_a_idx'::regclass) =
       bm25_level_counts('compaction_step_b_idx'::regclass)
       AS step_matches_full_compact;

-- Only the owner can run compaction control functions.
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'compaction_user') THEN
        EXECUTE 'REASSIGN OWNED BY compaction_user TO CURRENT_USER';
        EXECUTE 'DROP OWNED BY compaction_user CASCADE';
        DROP ROLE compaction_user;
    END IF;
END $$;
CREATE ROLE compaction_user LOGIN;
GRANT USAGE ON SCHEMA public TO compaction_user;
SET ROLE compaction_user;
SELECT bm25_compact('compaction_step_a_idx'::regclass);
SELECT bm25_compact_step('compaction_step_a_idx'::regclass);
RESET ROLE;
DROP OWNED BY compaction_user CASCADE;
DROP ROLE compaction_user;

-- The sweeper compacts eligible indexes and leaves them below threshold.
SELECT bm25_compact('compaction_test_idx'::regclass);
SET pg_textsearch.segments_per_level = 64;
CREATE TABLE compaction_pending (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_pending_idx ON compaction_pending
    USING bm25(body) WITH (text_config = 'english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO compaction_pending (body)
        SELECT format('pending batch %s doc %s filler filler', n, i)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('compaction_pending_idx');
    END LOOP;
END $$;

SET pg_textsearch.segments_per_level = 2;
SELECT bm25_needs_compaction('compaction_pending_idx'::regclass)
       AS pending_needs_before;
SELECT bm25_compact_pending() = 1 AS pending_compacted_one;
SELECT NOT bm25_needs_compaction('compaction_pending_idx'::regclass)
       AS pending_needs_after;

-- A bad candidate warns, but the sweeper continues to the next index.
SET pg_textsearch.segments_per_level = 64;
CREATE TABLE compaction_resilient (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_resilient_idx ON compaction_resilient
    USING bm25(body) WITH (text_config = 'english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO compaction_resilient (body)
        SELECT format('resilient batch %s doc %s filler filler', n, i)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('compaction_resilient_idx');
    END LOOP;
END $$;

SET pg_textsearch.segments_per_level = 2;
-- Mirror the real function's pinned search_path, so the stub resolves
-- names the same way the function it stands in for does.  That means
-- the relation reference must be schema-qualified.
CREATE OR REPLACE FUNCTION bm25_indexes_needing_compaction()
RETURNS SETOF regclass
LANGUAGE sql STABLE
SET search_path = pg_catalog, pg_temp
AS $func$
    VALUES (0::oid::regclass),
           ('public.compaction_resilient_idx'::regclass);
$func$;
SELECT bm25_compact_pending() = 1 AS pending_continued_after_warning;
SELECT NOT bm25_needs_compaction('compaction_resilient_idx'::regclass)
       AS pending_resilience_completed;

DROP TABLE compaction_resilient CASCADE;
DROP TABLE compaction_pending CASCADE;
DROP TABLE compaction_step_a CASCADE;
DROP TABLE compaction_step_b CASCADE;
DROP TABLE compaction_manual CASCADE;
DROP TABLE compaction_partitioned CASCADE;
DROP TABLE compaction_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
