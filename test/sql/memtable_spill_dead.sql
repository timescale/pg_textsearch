-- Regression tests for spill step 4: tp_memtable_mark_chain_dead
-- stamps every page of the spilled chain (including fragment
-- continuation pages) with TP_MEMTABLE_PAGE_FLAG_DEAD before
-- tp_spill_finalize unlinks the chain from the metapage.
-- Orphans are visible via bm25_memtable_dead_pages().

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Test 1: heap inserts, manual spill, dead orphan count matches
-- the live chain page count that existed immediately before spill.
CREATE TABLE memtable_spill_dead_t (id serial PRIMARY KEY, body text);
CREATE INDEX memtable_spill_dead_idx ON memtable_spill_dead_t
    USING bm25(body) WITH (text_config = 'english');

SELECT count(*) AS dead_pages_before_spill
FROM bm25_memtable_dead_pages('memtable_spill_dead_idx');

INSERT INTO memtable_spill_dead_t (body)
SELECT 'spill dead ' || i || ' ' || repeat('pad ', 6)
FROM generate_series(1, 200) i;

CREATE TEMP TABLE spill_dead_live_pages AS
SELECT count(*)::int AS n
FROM bm25_memtable_chain('memtable_spill_dead_idx');

SELECT n > 1 AS chain_multi_page FROM spill_dead_live_pages;

SELECT bm25_spill_index('memtable_spill_dead_idx') IS NOT NULL
       AS spill_ok;

SELECT count(*) AS live_chain_pages_after_spill
FROM bm25_memtable_chain('memtable_spill_dead_idx');

SELECT
    (SELECT count(*)::int
     FROM bm25_memtable_dead_pages('memtable_spill_dead_idx'))
    = (SELECT n FROM spill_dead_live_pages)
    AS dead_matches_spilled_chain;

SELECT bool_and((flags & 1) <> 0) AS all_dead_flag_set,
       bool_and(dead_fxid > 0) AS all_dead_fxid_set
FROM bm25_memtable_dead_pages('memtable_spill_dead_idx');

-- Second spill on an empty chain must not add more dead pages.
DO $$ BEGIN PERFORM bm25_spill_index('memtable_spill_dead_idx'); END $$;

SELECT
    (SELECT count(*)::int
     FROM bm25_memtable_dead_pages('memtable_spill_dead_idx'))
    = (SELECT n FROM spill_dead_live_pages)
    AS dead_count_unchanged_after_empty_respill;

DROP TABLE memtable_spill_dead_t;

-- Test 2: large INSERT forces fragment pages; spill stamps them DEAD.
CREATE TABLE memtable_spill_dead_frag_t (id int, body text);
CREATE INDEX memtable_spill_dead_frag_idx ON memtable_spill_dead_frag_t
    USING bm25(body) WITH (text_config = 'english');

INSERT INTO memtable_spill_dead_frag_t (body)
SELECT string_agg('tok' || gs::text || ' ', '')
FROM generate_series(1, 5000) gs;

CREATE TEMP TABLE frag_spill_live_pages AS
SELECT count(*)::int AS n
FROM bm25_memtable_chain('memtable_spill_dead_frag_idx');

SELECT n > 2 AS frag_chain_has_continuations FROM frag_spill_live_pages;

DO $$ BEGIN PERFORM bm25_spill_index('memtable_spill_dead_frag_idx'); END $$;

SELECT count(*) AS live_pages_after_frag_spill
FROM bm25_memtable_chain('memtable_spill_dead_frag_idx');

SELECT
    (SELECT count(*)::int
     FROM bm25_memtable_dead_pages('memtable_spill_dead_frag_idx'))
    = (SELECT n FROM frag_spill_live_pages)
    AS frag_dead_matches_spilled_chain;

SELECT bool_and((flags & 1) <> 0) AS frag_all_dead_flag,
       sum(((flags & 2) <> 0)::int)::int AS frag_dead_continuation_pages,
       sum(CASE WHEN (flags & 2) <> 0 THEN 0 ELSE 1 END)::int
           AS frag_dead_regular_pages
FROM bm25_memtable_dead_pages('memtable_spill_dead_frag_idx');

SELECT sum(((flags & 2) <> 0)::int)::int > 0
       AS frag_has_dead_continuation_pages
FROM bm25_memtable_dead_pages('memtable_spill_dead_frag_idx');

SELECT bool_and(
    CASE WHEN (flags & 2) <> 0 THEN n_records = 0
                                   ELSE n_records >= 0 END
) AS frag_dead_continuation_zero_records
FROM bm25_memtable_dead_pages('memtable_spill_dead_frag_idx');

DROP TABLE memtable_spill_dead_frag_t;
DROP EXTENSION pg_textsearch CASCADE;
