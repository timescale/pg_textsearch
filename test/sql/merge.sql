-- Test case: merge
-- Tests segment merge functionality and queries across multiple segment levels.
--
-- This test exercises:
-- 1. Multiple L0 segments
-- 2. L0 -> L1 merge triggered by segments_per_level = 2
-- 3. Queries across multiple segment levels (L0, L1)

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\set ECHO none
\i test/sql/validation.sql
\set ECHO all

SET pg_textsearch.log_scores = false;
SET enable_seqscan = off;

-- Enable merge: 2 segments per level triggers L0->L1 merge on 2nd spill
SET pg_textsearch.segments_per_level = 2;

-- Create table and index
CREATE TABLE merge_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE INDEX merge_test_idx ON merge_test USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

--------------------------------------------------------------------------------
-- Phase 1: First batch - will become first L0 segment after spill
--------------------------------------------------------------------------------

INSERT INTO merge_test (content) VALUES ('hello world database');
INSERT INTO merge_test (content) VALUES ('goodbye cruel world');
INSERT INTO merge_test (content) VALUES ('hello goodbye friend');
INSERT INTO merge_test (content) VALUES ('world peace harmony');

-- Verify data is queryable in memtable
SELECT 'Phase 1: memtable only' AS phase;
SELECT COUNT(*) AS count_before_spill FROM (
    SELECT id FROM merge_test
    ORDER BY content <@> to_bm25query('hello', 'merge_test_idx')
    LIMIT 100
) t;

-- First spill creates segment 1 in L0
SELECT bm25_spill_index('merge_test_idx') IS NOT NULL AS first_spill;

-- Verify data is still queryable from L0 segment
SELECT 'Phase 1b: after first spill (1 segment in L0)' AS phase;
SELECT COUNT(*) AS hello_count_after_first_spill FROM (
    SELECT id FROM merge_test
    ORDER BY content <@> to_bm25query('hello', 'merge_test_idx')
    LIMIT 100
) t;

--------------------------------------------------------------------------------
-- Phase 2: Second batch - triggers L0->L1 merge (segments_per_level=2)
--------------------------------------------------------------------------------

INSERT INTO merge_test (content) VALUES ('database indexing query');
INSERT INTO merge_test (content) VALUES ('search engine optimization');
INSERT INTO merge_test (content) VALUES ('database world news');
INSERT INTO merge_test (content) VALUES ('goodbye database friend');

-- Second spill creates segment 2 in L0, which triggers merge to L1
-- After merge: L0 is empty (cleared), L1 has merged segment
-- Note: Run with client_min_messages=debug1 to see merge DEBUG messages
SELECT bm25_spill_index('merge_test_idx') IS NOT NULL AS second_spill;

-- After merge: merged segment in L1, L0 is empty
SELECT 'Phase 2: after second spill + merge (L0 empty, 1 segment in L1)' AS phase;

-- Should find 2 documents with 'hello' (both in L1 merged segment)
SELECT COUNT(*) AS hello_count_after_merge FROM (
    SELECT id FROM merge_test
    ORDER BY content <@> to_bm25query('hello', 'merge_test_idx')
    LIMIT 100
) t;

-- Should find 4 documents with 'database' (all in L1 merged segment)
SELECT COUNT(*) AS database_count_after_merge FROM (
    SELECT id FROM merge_test
    ORDER BY content <@> to_bm25query('database', 'merge_test_idx')
    LIMIT 100
) t;

-- Validate BM25 scoring is correct across L1 data
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid_after_merge;
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'database', 'english', 1.2, 0.75)
       AS database_valid_after_merge;
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'world', 'english', 1.2, 0.75)
       AS world_valid_after_merge;

--------------------------------------------------------------------------------
-- Phase 3: Post-merge inserts (new memtable data + 1 L1 segment)
--------------------------------------------------------------------------------

INSERT INTO merge_test (content) VALUES ('hello new insertion');
INSERT INTO merge_test (content) VALUES ('database transaction log');

SELECT 'Phase 3: post-merge inserts (memtable + 1 L1 segment)' AS phase;

-- Should find 3 documents with 'hello' (2 in L1, 1 in memtable)
SELECT COUNT(*) AS hello_count_with_new_inserts FROM (
    SELECT id FROM merge_test
    ORDER BY content <@> to_bm25query('hello', 'merge_test_idx')
    LIMIT 100
) t;

-- Should find 5 documents with 'database' (4 in L1, 1 in memtable)
SELECT COUNT(*) AS database_count_with_new_inserts FROM (
    SELECT id FROM merge_test
    ORDER BY content <@> to_bm25query('database', 'merge_test_idx')
    LIMIT 100
) t;

-- Validate BM25 scoring with mixed sources (memtable + L1 segment)
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'hello', 'english', 1.2, 0.75)
       AS hello_valid_mixed;
SELECT validate_bm25_scoring('merge_test', 'content', 'merge_test_idx',
                             'database', 'english', 1.2, 0.75)
       AS database_valid_mixed;

--------------------------------------------------------------------------------
-- Phase 4: Verify total document count
--------------------------------------------------------------------------------

SELECT 'Phase 4: final verification' AS phase;
SELECT COUNT(*) AS total_documents FROM merge_test;

-- Show final scores for reference
SELECT id, content,
       ROUND((content <@> to_bm25query('database', 'merge_test_idx'))::numeric, 4)
       AS database_score
FROM merge_test
ORDER BY content <@> to_bm25query('database', 'merge_test_idx'), id;

--------------------------------------------------------------------------------
-- Phase 5: Persisted segment counts fail closed at capacity
--------------------------------------------------------------------------------

SET pg_textsearch.segments_per_level = 3;
SET pg_textsearch.debug_segment_count_limit = 2;

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

RESET pg_textsearch.debug_segment_count_limit;
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

SET pg_textsearch.debug_segment_count_limit = 2;
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

RESET pg_textsearch.debug_segment_count_limit;
RESET pg_textsearch.segments_per_level;
DROP TABLE merge_nested_full CASCADE;

--------------------------------------------------------------------------------
-- Phase 7: Bound ordinary compaction outputs
--------------------------------------------------------------------------------

SET pg_textsearch.segments_per_level = 4;
SET pg_textsearch.max_segment_size = '1MB';

CREATE TABLE merge_bounded (id bigint PRIMARY KEY, content text);
CREATE INDEX merge_bounded_idx ON merge_bounded USING bm25(content)
  WITH (text_config='simple');

DO $$
DECLARE
    batch integer;
BEGIN
    FOR batch IN 0..3 LOOP
        INSERT INTO merge_bounded
        SELECT batch * 2000 + gs,
               'common ' || repeat(md5((batch * 2000 + gs)::text), 4)
        FROM generate_series(1, 2000) gs;
        PERFORM bm25_spill_index('merge_bounded_idx');
    END LOOP;
END
$$;

DO $$
DECLARE
    summary text := bm25_summarize_index('merge_bounded_idx');
BEGIN
    IF regexp_count(summary, 'L1 Segment [0-9]+:') <> 2
       OR summary ~ 'L0 Segment' THEN
        RAISE EXCEPTION 'ordinary bounded compaction layout is wrong: %',
                        summary;
    END IF;
END
$$;

SELECT count(*) = 8000 AS bounded_merge_docs_preserved
FROM (
    SELECT 1 FROM merge_bounded
    ORDER BY content <@> to_bm25query('common', 'merge_bounded_idx')
    LIMIT 8000
) ranked;

RESET pg_textsearch.max_segment_size;
RESET pg_textsearch.segments_per_level;
DROP TABLE merge_bounded CASCADE;

-- Cleanup
DROP TABLE merge_test CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
