CREATE EXTENSION IF NOT EXISTS pg_textsearch;
\pset format unaligned
SET client_min_messages = warning;
SET pg_textsearch.segments_per_level = 64;
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 0;

-- Comparable spills are debt even below the per-level count threshold.
CREATE TEMP TABLE policy_small (id integer, body text);
CREATE INDEX policy_small_idx ON policy_small USING bm25(body)
    WITH (text_config = 'simple', compaction = 'manual');
DO $$
BEGIN
    FOR batch IN 1..4 LOOP
        INSERT INTO policy_small
        SELECT batch * 100 + n, 'common document ' || n
        FROM generate_series(1, 100) n;
        PERFORM bm25_spill_index('policy_small_idx');
    END LOOP;
    IF NOT bm25_needs_compaction('policy_small_idx') THEN
        RAISE EXCEPTION 'below-threshold small segments were not selected';
    END IF;
    PERFORM bm25_compact('policy_small_idx');
    IF (SELECT sum(n) FROM unnest(
            bm25_level_counts('policy_small_idx')) n) <> 1
       OR bm25_needs_compaction('policy_small_idx') THEN
        RAISE EXCEPTION 'small-segment consolidation did not converge';
    END IF;
END
$$;
SELECT count(*) = 400 AS small_merge_preserves_matches
FROM (SELECT id FROM policy_small
      ORDER BY body <@> to_bm25query('common', 'policy_small_idx')
      LIMIT 1000) ranked;

-- A tiny spill must not cause the much larger preceding output to rewrite.
CREATE TEMP TABLE policy_unequal (id integer, body text);
CREATE INDEX policy_unequal_idx ON policy_unequal USING bm25(body)
    WITH (text_config = 'simple', compaction = 'manual');
INSERT INTO policy_unequal
SELECT n, 'common ' || md5(n::text) FROM generate_series(1, 2000) n;
SELECT bm25_spill_index('policy_unequal_idx') > 0 AS large_spilled;
INSERT INTO policy_unequal VALUES (2001, 'common tiny');
SELECT bm25_spill_index('policy_unequal_idx') > 0 AS tiny_spilled;
SELECT NOT bm25_needs_compaction('policy_unequal_idx')
       AND NOT bm25_compact_step('policy_unequal_idx')
       AS unequal_segments_are_not_repeatedly_rewritten;

-- A singleton becomes eligible at 50% dead, not just at zero survivors.
CREATE TEMP TABLE policy_deleted (id integer, body text);
INSERT INTO policy_deleted
SELECT n, 'common document ' || n FROM generate_series(1, 100) n;
CREATE INDEX policy_deleted_idx ON policy_deleted USING bm25(body)
    WITH (text_config = 'simple', compaction = 'manual');
DELETE FROM policy_deleted WHERE id <= 49;
VACUUM policy_deleted;
SELECT NOT bm25_needs_compaction('policy_deleted_idx')
       AS minor_deletion_is_not_debt;
DELETE FROM policy_deleted WHERE id = 50;
VACUUM policy_deleted;
DO $$
BEGIN
    IF NOT bm25_needs_compaction('policy_deleted_idx')
       OR NOT bm25_compact_step('policy_deleted_idx') THEN
        RAISE EXCEPTION 'half-dead singleton was not rewritten';
    END IF;
    IF bm25_needs_compaction('policy_deleted_idx')
       OR bm25_compact_step('policy_deleted_idx') THEN
        RAISE EXCEPTION 'clean singleton was rewritten again';
    END IF;
END
$$;
SELECT array_agg(id ORDER BY id) = ARRAY(
           SELECT generate_series(51, 100)) AS rewrite_preserves_survivors
FROM (SELECT id FROM policy_deleted
      ORDER BY body <@> to_bm25query('common', 'policy_deleted_idx')
      LIMIT 1000) ranked;

-- Inline maintenance gets an opportunity after VACUUM, without a new spill.
CREATE TEMP TABLE policy_vacuum (id integer, body text);
INSERT INTO policy_vacuum
SELECT n, 'common document ' || n FROM generate_series(1, 100) n;
CREATE INDEX policy_vacuum_idx ON policy_vacuum USING bm25(body)
    WITH (text_config = 'simple');
DELETE FROM policy_vacuum WHERE id <= 75;
VACUUM policy_vacuum;
SELECT bm25_pending_free_pages('policy_vacuum_idx') > 0
       AND NOT bm25_needs_compaction('policy_vacuum_idx')
       AND NOT bm25_compact_step('policy_vacuum_idx')
       AS vacuum_runs_inline_policy;
SELECT count(*) = 25 AS vacuum_policy_preserves_survivors
FROM (SELECT id FROM policy_vacuum
      ORDER BY body <@> to_bm25query('common', 'policy_vacuum_idx')
      LIMIT 1000) ranked;
DELETE FROM policy_deleted;
VACUUM policy_deleted;
SELECT (SELECT sum(n) FROM unnest(
            bm25_level_counts('policy_deleted_idx')) n) = 0
       AS empty_segment_cleanup_preserved;

-- Sparse cleanup can splice out an interior segment without merging its head.
CREATE TEMP TABLE policy_interior (id integer, body text);
CREATE INDEX policy_interior_idx ON policy_interior USING bm25(body)
    WITH (text_config = 'simple', compaction = 'manual');
INSERT INTO policy_interior
SELECT n, 'common document ' || n FROM generate_series(1, 100) n;
SELECT bm25_spill_index('policy_interior_idx') > 0 AS older_spilled;
INSERT INTO policy_interior
SELECT n, 'common document ' || n FROM generate_series(101, 200) n;
SELECT bm25_spill_index('policy_interior_idx') > 0 AS newer_spilled;
DELETE FROM policy_interior WHERE id <= 50;
VACUUM policy_interior;
CREATE TEMP TABLE policy_interior_before AS
SELECT (regexp_match(summary, 'L0 Segment 1: block=([0-9]+),'))[1]
           AS clean_root,
       (regexp_match(summary, 'L0 Segment 2: block=([0-9]+),'))[1]
           AS sparse_root
FROM (SELECT bm25_summarize_index('policy_interior_idx') AS summary) s;
SELECT bm25_compact_step('policy_interior_idx') AS interior_rewrite_ran;
SELECT bm25_level_counts('policy_interior_idx') =
           ARRAY[2, 0, 0, 0, 0, 0, 0, 0]
       AND bm25_summarize_index('policy_interior_idx')
           ~ E'total_docs: 150\n'
       AS interior_rewrite_does_not_copy_clean_head;
SELECT (regexp_match(summary, 'L0 Segment 1: block=([0-9]+),'))[1]
           = before.clean_root
       AND (regexp_match(summary, 'L0 Segment 2: block=([0-9]+),'))[1]
           <> before.sparse_root AS only_sparse_root_replaced
FROM policy_interior_before before,
     (SELECT bm25_summarize_index('policy_interior_idx') AS summary) s;
SELECT count(*) = 150 AS interior_rewrite_preserves_matches
FROM (SELECT id FROM policy_interior
      ORDER BY body <@> to_bm25query('common', 'policy_interior_idx')
      LIMIT 1000) ranked;

DROP TABLE policy_small, policy_unequal, policy_deleted, policy_vacuum,
    policy_interior, policy_interior_before;

-- The managed worker's physical-target entry point sees the same small debt.
CREATE TABLE policy_background (id integer, body text);
CREATE INDEX policy_background_idx ON policy_background USING bm25(body)
    WITH (text_config = 'simple', compaction = 'manual');
DO $$
BEGIN
    FOR batch IN 1..2 LOOP
        INSERT INTO policy_background
        SELECT batch * 100 + n, 'common document ' || n
        FROM generate_series(1, 100) n;
        PERFORM bm25_spill_index('policy_background_idx');
    END LOOP;
END
$$;
UPDATE pg_class SET reloptions = array_replace(
    reloptions, 'compaction=manual', 'compaction=background')
WHERE oid = 'policy_background_idx'::regclass;
SELECT bm25_compact_step_if_current(
           c.oid, d.oid, coalesce(nullif(c.reltablespace, 0), d.dattablespace),
           pg_relation_filenode(c.oid), c.relowner)
       AS background_target_compacts_below_threshold
FROM pg_class c JOIN pg_database d ON d.datname = current_database()
WHERE c.oid = 'policy_background_idx'::regclass;
SELECT NOT bm25_needs_compaction('policy_background_idx')
       AS background_target_converged;
DROP TABLE policy_background;
RESET pg_textsearch.segments_per_level;
RESET pg_textsearch.memtable_pages_threshold;
RESET pg_textsearch.bulk_load_threshold;
DROP EXTENSION pg_textsearch CASCADE;
