SET client_min_messages = warning;
CREATE EXTENSION pg_textsearch;
CREATE EXTENSION injection_points;
CREATE EXTENSION pg_textsearch_test;

SET pg_textsearch.segments_per_level = 3;
SELECT pg_textsearch_test_attach_segment_limit(2);

CREATE TABLE merge_capacity (id serial PRIMARY KEY, content text);
CREATE INDEX merge_capacity_idx ON merge_capacity USING bm25(content)
  WITH (text_config='english', compaction='manual');

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

DO $$
BEGIN
    BEGIN
        PERFORM bm25_spill_index('merge_capacity_idx');
        RAISE EXCEPTION 'expected segment count limit error';
    EXCEPTION
        WHEN program_limit_exceeded THEN
            IF SQLERRM <> 'bm25 segment count limit reached at level 0' THEN
                RAISE;
            END IF;
    END;
END
$$;

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

CREATE TABLE merge_nested_full (id serial PRIMARY KEY, content text);
CREATE INDEX merge_nested_full_idx ON merge_nested_full USING bm25(content)
  WITH (text_config='english', compaction='manual');

-- Construct two full destination levels without relying on tiny outputs
-- being promoted repeatedly.
DO $$
DECLARE
    n integer;
BEGIN
    FOR level IN REVERSE 2..1 LOOP
        FOR n IN 1..2 LOOP
            INSERT INTO merge_nested_full (content)
            SELECT format('nested full document %s filler', gs)
            FROM generate_series(1, 6) gs;
            PERFORM bm25_spill_index('merge_nested_full_idx');
        END LOOP;
        PERFORM pg_textsearch_test_move_level(
            'merge_nested_full_idx', 0, level);
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

SELECT bm25_compact_step('merge_nested_full_idx');
DO $$
DECLARE
    summary text := bm25_summarize_index('merge_nested_full_idx');
BEGIN
    IF regexp_count(summary, 'L[0-7] Segment [0-9]+:') <> 4
       OR summary !~ 'L1 Segment 2:'
       OR summary !~ 'L2 Segment 2:'
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

SELECT bm25_compact('merge_nested_full_idx');
SELECT bm25_level_counts('merge_nested_full_idx') =
           ARRAY[0, 1, 0, 0, 0, 0, 0, 0]
       AND NOT bm25_needs_compaction('merge_nested_full_idx')
       AS capacity_recourse_converges_without_promotion;

SELECT injection_points_detach(
           'pg-textsearch-segment-count-limit');
RESET pg_textsearch.segments_per_level;
DROP TABLE merge_nested_full CASCADE;
DROP EXTENSION pg_textsearch_test;
DROP EXTENSION injection_points;
DROP EXTENSION pg_textsearch CASCADE;
