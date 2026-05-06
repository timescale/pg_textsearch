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

-- ----------------------------------------------------------------
-- Coverage for shrinkage in the GenericXLog merge fallback
--
-- The else-branch of merge.c also has to subtract dead-doc count
-- and total_tokens from the in-memory atomics and the on-disk
-- metapage when V5 alive-bitset shrinkage drops docs at merge
-- time. Without a delete+vacuum sequence the shrinkage values
-- are zero and the `if (docs_shrinkage > 0)` / `if
-- (tokens_shrinkage > 0)` branches stay uncovered. Drive a
-- clean delete + VACUUM + force-merge here to exercise them.
--
-- Run with the default segments_per_level (= 8) so the L0 spills
-- here don't auto-compact: we want the merge to run only via the
-- explicit force-merge call after the alive-bitset bits have been
-- flipped dead by VACUUM.
-- ----------------------------------------------------------------
CREATE UNLOGGED TABLE unlogged_shrink (
    id SERIAL PRIMARY KEY,
    content TEXT
);
CREATE INDEX unlogged_shrink_idx ON unlogged_shrink USING bm25(content)
    WITH (text_config='english');

-- Two segments at L0 with disjoint content.
INSERT INTO unlogged_shrink (content)
    SELECT 'foo doc ' || i FROM generate_series(1, 100) i;
SELECT bm25_spill_index('unlogged_shrink_idx') > 0 AS s1_wrote;

INSERT INTO unlogged_shrink (content)
    SELECT 'bar doc ' || i FROM generate_series(101, 200) i;
SELECT bm25_spill_index('unlogged_shrink_idx') > 0 AS s2_wrote;

-- Delete half the docs. VACUUM marks the alive-bitset bits dead
-- in each segment. The merge below then drops them, producing
-- non-zero docs_shrinkage and tokens_shrinkage.
DELETE FROM unlogged_shrink WHERE id <= 50 OR (id > 100 AND id <= 150);
VACUUM unlogged_shrink;

-- Force-merge: combines the two L0 segments and drops the
-- alive-bitset-dead docs. This is the GenericXLog else-branch
-- since the index is UNLOGGED, and shrinkage > 0, so the
-- atomic-sub and metapage-sub branches both fire.
SELECT bm25_force_merge('unlogged_shrink_idx');

-- Survivor count should be 100 (half of 200). The shrinkage
-- math is right when this matches; if the metapage sub
-- underflowed or the atomic sub miscounted, the corpus stats
-- would diverge from the actual survivors.
SELECT bm25_summarize_index('unlogged_shrink_idx') ~ 'total_docs: 100'
    AS total_docs_correct;

-- Cleanup
DROP TABLE unlogged_shrink;
DROP TABLE unlogged_merge;
DROP TABLE unlogged_test;
DROP EXTENSION pg_textsearch CASCADE;