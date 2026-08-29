-- Test case: force_merge
-- Tests bm25_force_merge() which merges all segments into one.
--
-- This test exercises:
-- 1. Multiple segments across levels
-- 2. Force merge into a single segment
-- 3. Queries return correct results after merge
-- 4. Inserts work normally after force merge

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\set ECHO none
\i test/sql/validation.sql
\set ECHO all

SET pg_textsearch.log_scores = false;
SET enable_seqscan = off;

-- Use 2 segments per level so we get multi-level segments quickly
SET pg_textsearch.segments_per_level = 2;

CREATE TABLE force_merge_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE INDEX force_merge_idx ON force_merge_test USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Empty indexes are already a valid zero-segment force-merge result.
DO $$
DECLARE
    summary text;
BEGIN
    PERFORM bm25_force_merge('force_merge_idx');
    summary := bm25_summarize_index('force_merge_idx');
    IF summary !~ E'Segments:\n  \\(none\\)' THEN
        RAISE EXCEPTION 'empty force merge created a segment: %', summary;
    END IF;
END
$$;

--------------------------------------------------------------------------------
-- Build up segments across multiple levels
--------------------------------------------------------------------------------

-- Batch 1 -> spill to L0
INSERT INTO force_merge_test (content) VALUES
    ('hello world database'),
    ('goodbye cruel world'),
    ('hello goodbye friend'),
    ('world peace harmony');
SELECT bm25_spill_index('force_merge_idx') IS NOT NULL AS spill1;

-- Batch 2 -> spill to L0, triggers L0->L1 merge
INSERT INTO force_merge_test (content) VALUES
    ('database indexing query'),
    ('search engine optimization'),
    ('database world news'),
    ('goodbye database friend');
SELECT bm25_spill_index('force_merge_idx') IS NOT NULL AS spill2;

-- Batch 3 -> new L0 segment (so we have L0 + L1)
INSERT INTO force_merge_test (content) VALUES
    ('hello search results'),
    ('optimization database tuning');
SELECT bm25_spill_index('force_merge_idx') IS NOT NULL AS spill3;

-- Verify counts before force merge
SELECT COUNT(*) AS hello_before FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('hello', 'force_merge_idx')
    LIMIT 100
) t;

SELECT COUNT(*) AS database_before FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('database', 'force_merge_idx')
    LIMIT 100
) t;

--------------------------------------------------------------------------------
-- Force merge all segments into one
--------------------------------------------------------------------------------

SELECT bm25_force_merge('force_merge_idx');

DO $$
DECLARE
    summary text := bm25_summarize_index('force_merge_idx');
BEGIN
    IF regexp_count(summary, 'L[0-7] Segment [0-9]+:') <> 1 THEN
        RAISE EXCEPTION 'force merge left multiple segments: %', summary;
    END IF;
END
$$;

-- Verify same counts after force merge
SELECT COUNT(*) AS hello_after FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('hello', 'force_merge_idx')
    LIMIT 100
) t;

SELECT COUNT(*) AS database_after FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('database', 'force_merge_idx')
    LIMIT 100
) t;

-- Validate BM25 scoring is correct after force merge
SELECT validate_bm25_scoring('force_merge_test', 'content',
                             'force_merge_idx', 'hello', 'english',
                             1.2, 0.75) AS hello_scores_valid;
SELECT validate_bm25_scoring('force_merge_test', 'content',
                             'force_merge_idx', 'database', 'english',
                             1.2, 0.75) AS database_scores_valid;

--------------------------------------------------------------------------------
-- Verify inserts work after force merge
--------------------------------------------------------------------------------

INSERT INTO force_merge_test (content) VALUES
    ('hello after merge'),
    ('new database entry');

SELECT COUNT(*) AS hello_with_new FROM (
    SELECT id FROM force_merge_test
    ORDER BY content <@> to_bm25query('hello', 'force_merge_idx')
    LIMIT 100
) t;

SELECT validate_bm25_scoring('force_merge_test', 'content',
                             'force_merge_idx', 'hello', 'english',
                             1.2, 0.75) AS hello_valid_after_insert;

DROP TABLE force_merge_test CASCADE;

--------------------------------------------------------------------------------
-- A force-merge spill must not run ordinary threshold compaction first.
--
-- The POC fixture used 128 L0 segments with compaction disabled. PR 1 has no
-- scheduler mode, so this is the smallest equivalent carry: two L0 segments
-- built at threshold 3, then a pending memtable and threshold 2.
--------------------------------------------------------------------------------

SET pg_textsearch.segments_per_level = 3;

CREATE TABLE force_spill_cascade (id serial PRIMARY KEY, content text);
CREATE INDEX force_spill_cascade_idx ON force_spill_cascade USING bm25(content)
  WITH (text_config='english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..2 LOOP
        INSERT INTO force_spill_cascade (content)
        VALUES (format('spill cascade document %s filler', n));
        PERFORM bm25_spill_index('force_spill_cascade_idx');
    END LOOP;
    INSERT INTO force_spill_cascade (content)
    VALUES ('spill cascade pending memtable document filler');
END
$$;

DO $$
DECLARE
    summary text := bm25_summarize_index('force_spill_cascade_idx');
BEGIN
    IF regexp_count(summary, 'L[0-7] Segment [0-9]+:') <> 2
       OR summary !~ 'L0 Segment 2:'
       OR summary !~ E'Memtable:\n  terms: 0\n  documents: 1' THEN
        RAISE EXCEPTION 'force spill cascade layout was not constructed: %',
                        summary;
    END IF;
END
$$;

SET pg_textsearch.debug_segment_count_limit = 2;
DO $$
DECLARE
    summary_before text := bm25_summarize_index('force_spill_cascade_idx');
BEGIN
    BEGIN
        PERFORM bm25_force_merge('force_spill_cascade_idx');
        RAISE EXCEPTION 'capacity-overflowing force merge was accepted';
    EXCEPTION
        WHEN program_limit_exceeded THEN NULL;
    END;

    IF bm25_summarize_index('force_spill_cascade_idx') <> summary_before THEN
        RAISE EXCEPTION 'force merge capacity rejection mutated the index';
    END IF;
END
$$;
RESET pg_textsearch.debug_segment_count_limit;

SET pg_textsearch.segments_per_level = 2;
DO $$
BEGIN
    PERFORM bm25_force_merge('force_spill_cascade_idx');
END
$$;

DO $$
DECLARE
    summary text := bm25_summarize_index('force_spill_cascade_idx');
BEGIN
    IF regexp_count(summary, 'L[0-7] Segment [0-9]+:') <> 1
       OR summary !~ E'Memtable:\n  terms: 0\n  documents: 0' THEN
        RAISE EXCEPTION 'force spill cascade did not reach one segment: %',
                        summary;
    END IF;

    IF (SELECT count(*) FROM (
            SELECT 1
            FROM force_spill_cascade
            ORDER BY content <@>
                     to_bm25query('filler', 'force_spill_cascade_idx')
        ) ranked) <> 3 THEN
        RAISE EXCEPTION 'force spill cascade lost documents';
    END IF;
END
$$;

DROP TABLE force_spill_cascade CASCADE;

--------------------------------------------------------------------------------
-- Terminal L7 layouts reject before mutation.
--------------------------------------------------------------------------------

SET pg_textsearch.segments_per_level = 2;

CREATE TABLE force_l7_single (id serial PRIMARY KEY, content text);
CREATE INDEX force_l7_single_idx ON force_l7_single USING bm25(content)
  WITH (text_config='english');
CREATE TABLE force_l7_multiple (id serial PRIMARY KEY, content text);
CREATE INDEX force_l7_multiple_idx ON force_l7_multiple USING bm25(content)
  WITH (text_config='english');
CREATE TABLE force_l7_mixed (id serial PRIMARY KEY, content text);
CREATE INDEX force_l7_mixed_idx ON force_l7_mixed USING bm25(content)
  WITH (text_config='english');
CREATE TABLE force_l7_memtable (id serial PRIMARY KEY, content text);
CREATE INDEX force_l7_memtable_idx ON force_l7_memtable USING bm25(content)
  WITH (text_config='english');

DO $$
DECLARE
    n integer;
BEGIN
    FOR n IN 1..256 LOOP
        INSERT INTO force_l7_multiple (content)
        VALUES (format('multiple terminal %s filler', n));
        PERFORM bm25_spill_index('force_l7_multiple_idx');

        IF n <= 128 THEN
            INSERT INTO force_l7_single (content)
            VALUES (format('single terminal %s filler', n));
            PERFORM bm25_spill_index('force_l7_single_idx');
            INSERT INTO force_l7_mixed (content)
            VALUES (format('mixed terminal %s filler', n));
            PERFORM bm25_spill_index('force_l7_mixed_idx');
            INSERT INTO force_l7_memtable (content)
            VALUES (format('memtable terminal %s filler', n));
            PERFORM bm25_spill_index('force_l7_memtable_idx');
        END IF;
    END LOOP;

    INSERT INTO force_l7_mixed (content) VALUES ('mixed lower filler');
    PERFORM bm25_spill_index('force_l7_mixed_idx');
    INSERT INTO force_l7_memtable (content) VALUES ('memtable pending filler');
END
$$;

DO $$
DECLARE
    single_summary text := bm25_summarize_index('force_l7_single_idx');
    multiple_summary text := bm25_summarize_index('force_l7_multiple_idx');
    mixed_summary text := bm25_summarize_index('force_l7_mixed_idx');
    memtable_summary text := bm25_summarize_index('force_l7_memtable_idx');
BEGIN
    IF regexp_count(single_summary, 'L[0-7] Segment [0-9]+:') <> 1
       OR single_summary !~ 'L7 Segment 1:' THEN
        RAISE EXCEPTION 'single L7 layout was not constructed: %',
                        single_summary;
    END IF;
    IF regexp_count(multiple_summary, 'L[0-7] Segment [0-9]+:') <> 2
       OR multiple_summary !~ 'L7 Segment 2:' THEN
        RAISE EXCEPTION 'multiple L7 layout was not constructed: %',
                        multiple_summary;
    END IF;
    IF regexp_count(mixed_summary, 'L[0-7] Segment [0-9]+:') <> 2
       OR mixed_summary !~ 'L0 Segment 1:'
       OR mixed_summary !~ 'L7 Segment 1:' THEN
        RAISE EXCEPTION 'mixed L7 layout was not constructed: %',
                        mixed_summary;
    END IF;
    IF regexp_count(memtable_summary, 'L[0-7] Segment [0-9]+:') <> 1
       OR memtable_summary !~ 'L7 Segment 1:'
       OR memtable_summary !~ E'Memtable:\n  terms: 0\n  documents: 1' THEN
        RAISE EXCEPTION 'memtable L7 layout was not constructed: %',
                        memtable_summary;
    END IF;
END
$$;

DO $$
DECLARE
    single_before text := bm25_summarize_index('force_l7_single_idx');
    multiple_before text := bm25_summarize_index('force_l7_multiple_idx');
    mixed_before text := bm25_summarize_index('force_l7_mixed_idx');
    memtable_before text := bm25_summarize_index('force_l7_memtable_idx');
BEGIN
    PERFORM bm25_force_merge('force_l7_single_idx');

    BEGIN
        PERFORM bm25_force_merge('force_l7_multiple_idx');
        RAISE EXCEPTION 'multiple L7 force merge was accepted';
    EXCEPTION
        WHEN program_limit_exceeded THEN NULL;
    END;

    BEGIN
        PERFORM bm25_force_merge('force_l7_mixed_idx');
        RAISE EXCEPTION 'mixed L7 force merge was accepted';
    EXCEPTION
        WHEN program_limit_exceeded THEN NULL;
    END;

    BEGIN
        PERFORM bm25_force_merge('force_l7_memtable_idx');
        RAISE EXCEPTION 'memtable L7 force merge was accepted';
    EXCEPTION
        WHEN program_limit_exceeded THEN NULL;
    END;

    IF bm25_summarize_index('force_l7_single_idx') <> single_before
       OR bm25_summarize_index('force_l7_multiple_idx') <> multiple_before
       OR bm25_summarize_index('force_l7_mixed_idx') <> mixed_before
       OR bm25_summarize_index('force_l7_memtable_idx') <>
          memtable_before THEN
        RAISE EXCEPTION 'terminal force merge changed physical state';
    END IF;

    IF (SELECT count(*) FROM (
            SELECT 1 FROM force_l7_single
            ORDER BY content <@>
                     to_bm25query('filler', 'force_l7_single_idx')
        ) ranked) <> 128
       OR (SELECT count(*) FROM (
               SELECT 1 FROM force_l7_multiple
               ORDER BY content <@>
                        to_bm25query('filler', 'force_l7_multiple_idx')
           ) ranked) <> 256
       OR (SELECT count(*) FROM (
               SELECT 1 FROM force_l7_mixed
               ORDER BY content <@>
                        to_bm25query('filler', 'force_l7_mixed_idx')
           ) ranked) <> 129
       OR (SELECT count(*) FROM (
               SELECT 1 FROM force_l7_memtable
               ORDER BY content <@>
                        to_bm25query('filler', 'force_l7_memtable_idx')
           ) ranked) <> 129 THEN
        RAISE EXCEPTION 'terminal force merge lost documents';
    END IF;
END
$$;

DROP TABLE force_l7_single CASCADE;
DROP TABLE force_l7_multiple CASCADE;
DROP TABLE force_l7_mixed CASCADE;
DROP TABLE force_l7_memtable CASCADE;

--------------------------------------------------------------------------------
-- Cleanup
--------------------------------------------------------------------------------

RESET pg_textsearch.segments_per_level;
DROP EXTENSION pg_textsearch CASCADE;
