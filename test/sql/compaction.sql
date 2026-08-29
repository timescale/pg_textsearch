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

-- Persisted level counts fail closed before the metapage swap is published.
SET pg_textsearch.segments_per_level = 2;
SET pg_textsearch.compaction_mode = 'off';
SET pg_textsearch.debug_segment_count_limit = 2;

CREATE TABLE segment_count_l0 (id serial PRIMARY KEY, body text);
INSERT INTO segment_count_l0 (body)
SELECT 'initial build document ' || i || ' filler filler'
FROM generate_series(1, 20) i;
CREATE INDEX segment_count_l0_idx ON segment_count_l0
    USING bm25(body) WITH (text_config = 'english');

INSERT INTO segment_count_l0 (body)
SELECT 'second L0 segment document ' || i || ' filler filler'
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('segment_count_l0_idx') IS NOT NULL
       AS segment_count_l0_second;

CREATE TEMP TABLE segment_count_l0_before AS
SELECT counts[1] AS l0_count,
       (regexp_match(summary, 'L0 Segment 1: block=([0-9]+),'))[1]::bigint
           AS l0_head
FROM (
    SELECT bm25_level_counts('segment_count_l0_idx'::regclass) AS counts,
           bm25_summarize_index('segment_count_l0_idx') AS summary
) state;

INSERT INTO segment_count_l0 (body)
SELECT 'rejected L0 segment document ' || i || ' filler filler'
FROM generate_series(1, 20) i;
CREATE TEMP TABLE segment_count_l0_size_before AS
SELECT pg_relation_size('segment_count_l0_idx') AS relation_size;
SELECT bm25_spill_index('segment_count_l0_idx');

WITH current_state AS (
    SELECT bm25_level_counts('segment_count_l0_idx'::regclass) AS counts,
           bm25_summarize_index('segment_count_l0_idx') AS summary
)
SELECT current_state.counts[1] = before.l0_count
       AND
       (regexp_match(
           current_state.summary,
           'L0 Segment 1: block=([0-9]+),'))[1]::bigint = before.l0_head
       AS segment_count_l0_unchanged
FROM current_state, segment_count_l0_before before;

SELECT pg_relation_size('segment_count_l0_idx') = before.relation_size
       AS segment_count_l0_size_unchanged
FROM segment_count_l0_size_before before;

RESET pg_textsearch.debug_segment_count_limit;
DROP TABLE segment_count_l0 CASCADE;

SET pg_textsearch.debug_segment_count_limit = 2;
CREATE TABLE segment_count_step (id serial PRIMARY KEY, body text);
CREATE TABLE segment_count_full (id serial PRIMARY KEY, body text);
CREATE TABLE segment_count_force (id serial PRIMARY KEY, body text);
CREATE INDEX segment_count_step_idx ON segment_count_step
    USING bm25(body) WITH (text_config = 'english');
CREATE INDEX segment_count_full_idx ON segment_count_full
    USING bm25(body) WITH (text_config = 'english');
CREATE INDEX segment_count_force_idx ON segment_count_force
    USING bm25(body) WITH (text_config = 'english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO segment_count_step (body)
        SELECT format(
            'recovery batch %s first segment document %s filler', n, i)
        FROM generate_series(1, 20) i;
        INSERT INTO segment_count_full (body)
        SELECT format(
            'recovery batch %s first segment document %s filler', n, i)
        FROM generate_series(1, 20) i;
        INSERT INTO segment_count_force (body)
        SELECT format(
            'recovery batch %s first segment document %s filler', n, i)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('segment_count_step_idx');
        PERFORM bm25_spill_index('segment_count_full_idx');
        PERFORM bm25_spill_index('segment_count_force_idx');

        INSERT INTO segment_count_step (body)
        SELECT format(
            'recovery batch %s second segment document %s filler', n, i)
        FROM generate_series(1, 20) i;
        INSERT INTO segment_count_full (body)
        SELECT format(
            'recovery batch %s second segment document %s filler', n, i)
        FROM generate_series(1, 20) i;
        INSERT INTO segment_count_force (body)
        SELECT format(
            'recovery batch %s second segment document %s filler', n, i)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('segment_count_step_idx');
        PERFORM bm25_spill_index('segment_count_full_idx');
        PERFORM bm25_spill_index('segment_count_force_idx');
        PERFORM bm25_compact_step('segment_count_step_idx'::regclass);
        PERFORM bm25_compact_step('segment_count_full_idx'::regclass);
        PERFORM bm25_compact_step('segment_count_force_idx'::regclass);
    END LOOP;

    FOR n IN 1..2 LOOP
        INSERT INTO segment_count_step (body)
        SELECT format(
            'blocked source %s document %s filler', n, i)
        FROM generate_series(1, 20) i;
        INSERT INTO segment_count_full (body)
        SELECT format(
            'blocked source %s document %s filler', n, i)
        FROM generate_series(1, 20) i;
        INSERT INTO segment_count_force (body)
        SELECT format(
            'blocked source %s document %s filler', n, i)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('segment_count_step_idx');
        PERFORM bm25_spill_index('segment_count_full_idx');
        PERFORM bm25_spill_index('segment_count_force_idx');
    END LOOP;
END $$;

SELECT bm25_level_counts('segment_count_step_idx'::regclass) =
           ARRAY[2, 2, 0, 0, 0, 0, 0, 0]
       AND
       bm25_level_counts('segment_count_full_idx'::regclass) =
           ARRAY[2, 2, 0, 0, 0, 0, 0, 0]
       AND
       bm25_level_counts('segment_count_force_idx'::regclass) =
           ARRAY[2, 2, 0, 0, 0, 0, 0, 0]
       AS segment_count_recovery_starts_blocked;

SELECT bm25_compact_step('segment_count_step_idx'::regclass)
       AS segment_count_step_frees_destination;
SELECT bm25_level_counts('segment_count_step_idx'::regclass) =
           ARRAY[2, 0, 1, 0, 0, 0, 0, 0]
       AS segment_count_step_exactly_one_batch;
SELECT bm25_compact_step('segment_count_step_idx'::regclass)
       AS segment_count_step_retries_lower;
SELECT bm25_level_counts('segment_count_step_idx'::regclass) =
           ARRAY[0, 1, 1, 0, 0, 0, 0, 0]
       AND NOT bm25_needs_compaction(
           'segment_count_step_idx'::regclass)
       AS segment_count_step_recovers;

SELECT bm25_compact('segment_count_full_idx'::regclass);
SELECT bm25_level_counts('segment_count_full_idx'::regclass) =
           ARRAY[0, 1, 1, 0, 0, 0, 0, 0]
       AND NOT bm25_needs_compaction(
           'segment_count_full_idx'::regclass)
       AS segment_count_full_recovers;

SELECT count(*) = 120 AS segment_count_step_preserves_documents
FROM (
    SELECT 1
    FROM segment_count_step
    ORDER BY body <@> to_bm25query('filler', 'segment_count_step_idx')
) ranked;
SELECT count(*) = 120 AS segment_count_full_preserves_documents
FROM (
    SELECT 1
    FROM segment_count_full
    ORDER BY body <@> to_bm25query('filler', 'segment_count_full_idx')
) ranked;

SELECT bm25_force_merge('segment_count_force_idx');
SELECT (
           SELECT pg_catalog.sum(segment_count)
           FROM pg_catalog.unnest(
               bm25_level_counts('segment_count_force_idx'::regclass))
               AS counts(segment_count)
       ) = 1 AS segment_count_force_single_segment;
SELECT count(*) = 120 AS segment_count_force_preserves_documents
FROM (
    SELECT 1
    FROM segment_count_force
    ORDER BY body <@> to_bm25query('filler', 'segment_count_force_idx')
) ranked;

RESET pg_textsearch.debug_segment_count_limit;
DROP TABLE segment_count_step CASCADE;
DROP TABLE segment_count_full CASCADE;
DROP TABLE segment_count_force CASCADE;

-- A full noncompactable L7 leaves L6 promotion safely blocked.
CREATE TABLE segment_count_terminal (id serial PRIMARY KEY, body text);
CREATE INDEX segment_count_terminal_idx ON segment_count_terminal
    USING bm25(body) WITH (text_config = 'english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..384 LOOP
        INSERT INTO segment_count_terminal (body)
        VALUES (format('terminal segment document %s filler', n));
        PERFORM bm25_spill_index('segment_count_terminal_idx');
    END LOOP;

    FOR n IN 1..380 LOOP
        IF NOT bm25_compact_step('segment_count_terminal_idx'::regclass) THEN
            RAISE EXCEPTION 'terminal level setup stopped at step %', n;
        END IF;
    END LOOP;
END $$;

SET pg_textsearch.debug_segment_count_limit = 2;
SELECT bm25_level_counts('segment_count_terminal_idx'::regclass) =
           ARRAY[0, 0, 0, 0, 0, 0, 2, 2]
       AS segment_count_terminal_starts_blocked;
CREATE TEMP TABLE segment_count_terminal_before AS
SELECT bm25_level_counts('segment_count_terminal_idx'::regclass) AS counts,
       bm25_pending_free_pages('segment_count_terminal_idx')
           AS pending_free_pages;
SELECT pending_free_pages > 0
       AS segment_count_terminal_has_reclaimable_work
FROM segment_count_terminal_before;

SELECT bm25_compact_step('segment_count_terminal_idx'::regclass);
SELECT bm25_level_counts('segment_count_terminal_idx'::regclass) =
           before.counts
       AS segment_count_terminal_step_fails_closed
FROM segment_count_terminal_before before;
SELECT bm25_pending_free_pages('segment_count_terminal_idx') =
           before.pending_free_pages
       AS segment_count_terminal_rejects_before_reclaim
FROM segment_count_terminal_before before;

SELECT bm25_compact('segment_count_terminal_idx'::regclass);
SELECT bm25_level_counts('segment_count_terminal_idx'::regclass) =
           before.counts
       AS segment_count_terminal_full_fails_closed
FROM segment_count_terminal_before before;

SELECT bm25_force_merge('segment_count_terminal_idx');
SELECT bm25_level_counts('segment_count_terminal_idx'::regclass) =
           before.counts
       AS segment_count_terminal_force_fails_closed
FROM segment_count_terminal_before before;

SELECT count(*) = 384 AS segment_count_terminal_preserves_documents
FROM (
    SELECT 1
    FROM segment_count_terminal
    ORDER BY body <@> to_bm25query('filler', 'segment_count_terminal_idx')
) ranked;

RESET pg_textsearch.debug_segment_count_limit;
RESET pg_textsearch.compaction_mode;
DROP TABLE segment_count_terminal CASCADE;

-- Force merge rejects terminal layouts before spilling or merging.
SET pg_textsearch.segments_per_level = 2;
SET pg_textsearch.compaction_mode = 'off';

CREATE TABLE force_l7_single (id serial PRIMARY KEY, body text);
CREATE INDEX force_l7_single_idx ON force_l7_single
    USING bm25(body) WITH (text_config = 'english');
CREATE TABLE force_l7_multiple (id serial PRIMARY KEY, body text);
CREATE INDEX force_l7_multiple_idx ON force_l7_multiple
    USING bm25(body) WITH (text_config = 'english');
CREATE TABLE force_l7_mixed (id serial PRIMARY KEY, body text);
CREATE INDEX force_l7_mixed_idx ON force_l7_mixed
    USING bm25(body) WITH (text_config = 'english');
CREATE TABLE force_l7_memtable (id serial PRIMARY KEY, body text);
CREATE INDEX force_l7_memtable_idx ON force_l7_memtable
    USING bm25(body) WITH (text_config = 'english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..128 LOOP
        INSERT INTO force_l7_single (body)
        VALUES (format('single terminal %s filler', n));
        PERFORM bm25_spill_index('force_l7_single_idx');
        INSERT INTO force_l7_multiple (body)
        VALUES (format('multiple terminal %s filler', n));
        PERFORM bm25_spill_index('force_l7_multiple_idx');
        INSERT INTO force_l7_mixed (body)
        VALUES (format('mixed terminal %s filler', n));
        PERFORM bm25_spill_index('force_l7_mixed_idx');
        INSERT INTO force_l7_memtable (body)
        VALUES (format('memtable terminal %s filler', n));
        PERFORM bm25_spill_index('force_l7_memtable_idx');
    END LOOP;

    FOR n IN 1..127 LOOP
        PERFORM bm25_compact_step('force_l7_single_idx'::regclass);
        PERFORM bm25_compact_step('force_l7_multiple_idx'::regclass);
        PERFORM bm25_compact_step('force_l7_mixed_idx'::regclass);
        PERFORM bm25_compact_step('force_l7_memtable_idx'::regclass);
    END LOOP;

    FOR n IN 129..256 LOOP
        INSERT INTO force_l7_multiple (body)
        VALUES (format('multiple terminal %s filler', n));
        PERFORM bm25_spill_index('force_l7_multiple_idx');
    END LOOP;
    FOR n IN 128..254 LOOP
        PERFORM bm25_compact_step('force_l7_multiple_idx'::regclass);
    END LOOP;

    INSERT INTO force_l7_mixed (body) VALUES ('mixed lower filler');
    PERFORM bm25_spill_index('force_l7_mixed_idx');
    INSERT INTO force_l7_memtable (body) VALUES ('memtable pending filler');
END
$$;

SET pg_textsearch.debug_segment_count_limit = 2;
SELECT bm25_level_counts('force_l7_single_idx'::regclass) =
           ARRAY[0, 0, 0, 0, 0, 0, 0, 1]
       AND bm25_level_counts('force_l7_multiple_idx'::regclass) =
           ARRAY[0, 0, 0, 0, 0, 0, 0, 2]
       AND bm25_level_counts('force_l7_mixed_idx'::regclass) =
           ARRAY[1, 0, 0, 0, 0, 0, 0, 1]
       AND bm25_level_counts('force_l7_memtable_idx'::regclass) =
           ARRAY[0, 0, 0, 0, 0, 0, 0, 1]
       AS force_l7_preflight_layouts_ready;

CREATE TEMP TABLE force_l7_before AS
SELECT index_name,
       bm25_level_counts(index_name::regclass) AS counts,
       bm25_pending_free_pages(index_name) AS pending_free_pages,
       pg_relation_size(index_name::regclass) AS relation_size
FROM (VALUES
    ('force_l7_single_idx'),
    ('force_l7_multiple_idx'),
    ('force_l7_mixed_idx'),
    ('force_l7_memtable_idx')
) indexes(index_name);

CREATE FUNCTION pg_temp.force_merge_rejected(
    index_name pg_catalog.text,
    expected_message pg_catalog.text)
RETURNS pg_catalog.bool
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
BEGIN
    PERFORM public.bm25_force_merge(index_name);
    RETURN false;
EXCEPTION
    WHEN program_limit_exceeded THEN
        RETURN SQLERRM = expected_message;
END
$$;

SELECT bm25_force_merge('force_l7_single_idx');
SELECT pg_temp.force_merge_rejected(
           'public.force_l7_multiple_idx',
           'cannot force merge "force_l7_multiple_idx": '
           'level 7 contains multiple segments')
       AS force_l7_multiple_rejected;
SELECT pg_temp.force_merge_rejected(
           'public.force_l7_mixed_idx',
           'cannot force merge "force_l7_mixed_idx": '
           'level 7 is occupied while lower levels remain')
       AS force_l7_mixed_rejected;
SELECT pg_temp.force_merge_rejected(
           'public.force_l7_memtable_idx',
           'cannot force merge "force_l7_memtable_idx": '
           'level 7 is occupied while the memtable remains nonempty')
       AS force_l7_memtable_rejected;

SELECT index_name,
       bm25_level_counts(index_name::regclass) = before.counts
       AND bm25_pending_free_pages(index_name) =
               before.pending_free_pages
       AND pg_relation_size(index_name::regclass) =
               before.relation_size
       AS force_l7_physical_state_unchanged
FROM force_l7_before before
ORDER BY index_name;

SELECT count(*) = 128 AS force_l7_single_preserves_documents
FROM (
    SELECT 1 FROM force_l7_single
    ORDER BY body <@> to_bm25query('filler', 'force_l7_single_idx')
) ranked;
SELECT count(*) = 256 AS force_l7_multiple_preserves_documents
FROM (
    SELECT 1 FROM force_l7_multiple
    ORDER BY body <@> to_bm25query('filler', 'force_l7_multiple_idx')
) ranked;
SELECT count(*) = 129 AS force_l7_mixed_preserves_documents
FROM (
    SELECT 1 FROM force_l7_mixed
    ORDER BY body <@> to_bm25query('filler', 'force_l7_mixed_idx')
) ranked;
SELECT count(*) = 129 AS force_l7_memtable_preserves_documents
FROM (
    SELECT 1 FROM force_l7_memtable
    ORDER BY body <@> to_bm25query('filler', 'force_l7_memtable_idx')
) ranked;

RESET pg_textsearch.debug_segment_count_limit;
DROP TABLE force_l7_single CASCADE;
DROP TABLE force_l7_multiple CASCADE;
DROP TABLE force_l7_mixed CASCADE;
DROP TABLE force_l7_memtable CASCADE;

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

-- Ownership drift warns, but the sweeper continues to the next index.
SET pg_textsearch.segments_per_level = 64;
CREATE ROLE compaction_sweeper LOGIN;
CREATE ROLE compaction_drift_owner LOGIN;
CREATE TABLE compaction_drift (id serial PRIMARY KEY, body text);
CREATE TABLE compaction_resilient (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_drift_idx ON compaction_drift
    USING bm25(body) WITH (text_config = 'english');
CREATE INDEX compaction_resilient_idx ON compaction_resilient
    USING bm25(body) WITH (text_config = 'english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO compaction_drift (body)
        SELECT format('drift batch %s doc %s filler filler', n, i)
        FROM generate_series(1, 20) i;
        INSERT INTO compaction_resilient (body)
        SELECT format('resilient batch %s doc %s filler filler', n, i)
        FROM generate_series(1, 20) i;
        PERFORM bm25_spill_index('compaction_drift_idx');
        PERFORM bm25_spill_index('compaction_resilient_idx');
    END LOOP;
END $$;

SET pg_textsearch.segments_per_level = 2;
ALTER TABLE compaction_drift OWNER TO compaction_drift_owner;
ALTER TABLE compaction_resilient OWNER TO compaction_sweeper;
SET ROLE compaction_sweeper;
SELECT NOT EXISTS (
           SELECT 1
           FROM bm25_indexes_needing_compaction() AS candidate(idx)
           WHERE candidate.idx = 'compaction_drift_idx'::regclass)
       AND EXISTS (
           SELECT 1
           FROM bm25_indexes_needing_compaction() AS candidate(idx)
           WHERE candidate.idx = 'compaction_resilient_idx'::regclass)
       AS pending_candidates_preserve_owner_visibility;
SELECT bm25_compact_pending() = 1 AS pending_continued_after_warning;
SELECT NOT bm25_needs_compaction('compaction_resilient_idx'::regclass)
       AS pending_resilience_completed;
RESET ROLE;

DROP TABLE compaction_drift CASCADE;
DROP TABLE compaction_resilient CASCADE;
DROP ROLE compaction_sweeper;
DROP ROLE compaction_drift_owner;
DROP TABLE compaction_pending CASCADE;
DROP TABLE compaction_step_a CASCADE;
DROP TABLE compaction_step_b CASCADE;
DROP TABLE compaction_manual CASCADE;
DROP TABLE compaction_partitioned CASCADE;
DROP TABLE compaction_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
