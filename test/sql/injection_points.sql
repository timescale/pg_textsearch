\pset format unaligned

SELECT current_setting(
           'pg_textsearch.debug_panic_after_spill_finalize',
           true) IS NULL AS panic_guc_removed;
SELECT current_setting(
           'pg_textsearch.debug_segment_count_limit',
           true) IS NULL AS segment_limit_guc_removed;

CREATE EXTENSION pg_textsearch;
CREATE EXTENSION injection_points;
CREATE EXTENSION pg_textsearch_test;

CREATE TABLE injection_capacity (
    id integer PRIMARY KEY,
    body text NOT NULL
);
CREATE INDEX injection_capacity_idx ON injection_capacity
    USING bm25(body) WITH (text_config = 'english', compaction = 'manual');

SELECT pg_textsearch_test_attach_segment_limit(2);

INSERT INTO injection_capacity VALUES (1, 'capacity alpha one');
SELECT bm25_spill_index('injection_capacity_idx') > 0 AS first_spill;
INSERT INTO injection_capacity VALUES (2, 'capacity alpha two');
SELECT bm25_spill_index('injection_capacity_idx') > 0 AS second_spill;
INSERT INTO injection_capacity VALUES (3, 'capacity alpha three');

DO $$
BEGIN
    BEGIN
        PERFORM bm25_spill_index('injection_capacity_idx');
        RAISE EXCEPTION 'expected segment count limit error';
    EXCEPTION
        WHEN program_limit_exceeded THEN
            IF SQLERRM <> 'bm25 segment count limit reached at level 0' THEN
                RAISE;
            END IF;
    END;
END
$$;

SELECT injection_points_detach(
           'pg-textsearch-segment-count-limit');
SELECT bm25_spill_index('injection_capacity_idx') > 0
       AS spill_after_detach;

DROP TABLE injection_capacity;
--------------------------------------------------------------------------------
-- Phase 5: Persisted segment counts fail closed at capacity
--------------------------------------------------------------------------------

SET pg_textsearch.segments_per_level = 3;
SELECT pg_textsearch_test_attach_segment_limit(2);

CREATE TABLE merge_capacity (id serial PRIMARY KEY, content text);
CREATE INDEX merge_capacity_idx ON merge_capacity USING bm25(content)
  WITH (text_config='english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO merge_capacity (content)
        VALUES (format('capacity document %s filler', n));
        PERFORM bm25_spill_index('merge_capacity_idx');
    END LOOP;
END
$$;

INSERT INTO merge_capacity (content)
VALUES ('capacity pending document filler');
SELECT bm25_spill_index('merge_capacity_idx');

DO $$
DECLARE
    summary text := bm25_summarize_index('merge_capacity_idx');
BEGIN
    IF regexp_count(summary, 'L[0-7] Segment [0-9]+:') <> 2
       OR summary !~ 'L0 Segment 2:'
       OR summary !~ E'Memtable:\n  terms: 0\n  documents: 1' THEN
        RAISE EXCEPTION 'segment capacity failure mutated the index: %',
                        summary;
    END IF;

    IF (SELECT count(*) FROM (
            SELECT 1
            FROM merge_capacity
            ORDER BY content <@>
                     to_bm25query('capacity', 'merge_capacity_idx')
        ) ranked) <> 3 THEN
        RAISE EXCEPTION 'segment capacity failure lost documents';
    END IF;
END
$$;

SELECT injection_points_detach(
           'pg-textsearch-segment-count-limit');
DROP TABLE merge_capacity CASCADE;

--------------------------------------------------------------------------------
-- Phase 6: Drain full destination levels before promoting the source
--------------------------------------------------------------------------------

SET pg_textsearch.segments_per_level = 3;

CREATE TABLE merge_nested_full (id serial PRIMARY KEY, content text);
CREATE INDEX merge_nested_full_idx ON merge_nested_full USING bm25(content)
  WITH (text_config='english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..24 LOOP
        INSERT INTO merge_nested_full (content)
        VALUES (format('nested full document %s filler', n));
        PERFORM bm25_spill_index('merge_nested_full_idx');
    END LOOP;
END
$$;

DO $$
DECLARE
    summary text := bm25_summarize_index('merge_nested_full_idx');
BEGIN
    IF regexp_count(summary, 'L[0-7] Segment [0-9]+:') <> 4
       OR summary !~ 'L1 Segment 2:'
       OR summary !~ 'L2 Segment 2:'
       OR summary ~ 'L0 Segment 1:' THEN
        RAISE EXCEPTION 'nested capacity layout was not constructed: %',
                        summary;
    END IF;
END
$$;

SELECT pg_textsearch_test_attach_segment_limit(2);
SET pg_textsearch.segments_per_level = 2;

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 25..26 LOOP
        INSERT INTO merge_nested_full (content)
        VALUES (format('nested full document %s filler', n));
        PERFORM bm25_spill_index('merge_nested_full_idx');
    END LOOP;
END
$$;

DO $$
DECLARE
    summary text := bm25_summarize_index('merge_nested_full_idx');
BEGIN
    IF regexp_count(summary, 'L[0-7] Segment [0-9]+:') <> 3
       OR summary !~ 'L1 Segment 1:'
       OR summary !~ 'L2 Segment 1:'
       OR summary !~ 'L3 Segment 1:'
       OR summary ~ 'L0 Segment 1:' THEN
        RAISE EXCEPTION 'nested capacity blockers were not drained: %',
                        summary;
    END IF;

    IF (SELECT count(*) FROM (
            SELECT 1
            FROM merge_nested_full
            ORDER BY content <@>
                     to_bm25query('nested', 'merge_nested_full_idx')
        ) ranked) <> 26 THEN
        RAISE EXCEPTION 'nested capacity compaction lost documents';
    END IF;
END
$$;

SELECT injection_points_detach(
           'pg-textsearch-segment-count-limit');
RESET pg_textsearch.segments_per_level;
DROP TABLE merge_nested_full CASCADE;

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
SELECT pg_textsearch_test_attach_segment_limit(2);
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

SELECT injection_points_detach(
           'pg-textsearch-segment-count-limit');
RESET pg_textsearch.segments_per_level;
DROP TABLE compaction_terminal CASCADE;

--------------------------------------------------------------------------------
-- Lowering the injected segment count limit after constructing a valid
-- topology cannot make force compaction or its pending spill fail.
--------------------------------------------------------------------------------

SET pg_textsearch.max_segment_size = '1MB';

CREATE TABLE force_lowered_capacity (
    id bigint PRIMARY KEY,
    content text
);
CREATE INDEX force_lowered_capacity_idx
  ON force_lowered_capacity USING bm25(content)
  WITH (text_config='simple');

DO $$
BEGIN
    FOR batch IN 1..9 LOOP
        INSERT INTO force_lowered_capacity
        SELECT batch,
               'capacitytoken ' ||
               string_agg(format('cap%sx%s', batch, term),
                          ' ' ORDER BY term)
        FROM generate_series(1, 12000) term;
        PERFORM bm25_spill_index('force_lowered_capacity_idx');
    END LOOP;
END
$$;

DO $$
DECLARE
    summary text := bm25_summarize_index('force_lowered_capacity_idx');
BEGIN
    IF regexp_count(summary, 'L[0-7] Segment [0-9]+:') <> 9
       OR summary !~ 'L0 Segment 9:' THEN
        RAISE EXCEPTION 'lowered-capacity layout was not constructed: %',
                        summary;
    END IF;
END
$$;

SELECT pg_textsearch_test_attach_segment_limit(1);
SELECT bm25_force_merge('force_lowered_capacity_idx');

INSERT INTO force_lowered_capacity
VALUES (10, 'capacitytoken pending spill');
SELECT bm25_force_merge('force_lowered_capacity_idx');

DO $$
DECLARE
    summary text := bm25_summarize_index('force_lowered_capacity_idx');
BEGIN
    IF regexp_count(summary, 'L[0-7] Segment [0-9]+:') <> 9
       OR summary !~ E'Memtable:\n  terms: 0\n  documents: 0' THEN
        RAISE EXCEPTION 'lowered-capacity force merge changed topology: %',
                        summary;
    END IF;

    IF (SELECT count(*) FROM (
            SELECT 1 FROM force_lowered_capacity
            ORDER BY content <@>
                     to_bm25query('capacitytoken',
                                  'force_lowered_capacity_idx')
        ) ranked) <> 10 THEN
        RAISE EXCEPTION 'lowered-capacity force merge lost documents';
    END IF;
END
$$;

SELECT injection_points_detach(
           'pg-textsearch-segment-count-limit');
RESET pg_textsearch.max_segment_size;
DROP TABLE force_lowered_capacity CASCADE;

DROP EXTENSION pg_textsearch_test;
DROP EXTENSION injection_points;
