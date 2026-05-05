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
-- Coverage for the GenericXLog merge-linkage fallback
--
-- merge.c routes the level-N → level-(N+1) linkage update through
-- tp_xlog_merge_linkage on WAL-logged indexes (custom rmgr, see
-- replication.h) and through GenericXLog on UNLOGGED / TEMP
-- indexes (no standby to confuse). The WAL-logged path is hit by
-- almost every other regression test that drives a merge; the
-- GenericXLog fallback is otherwise unexercised. Drive a real
-- compaction here to cover it.
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

-- Cleanup
DROP TABLE unlogged_merge;
DROP TABLE unlogged_test;
DROP EXTENSION pg_textsearch CASCADE;