\pset format unaligned
SET client_min_messages = warning;
CREATE EXTENSION pg_textsearch;
CREATE EXTENSION injection_points;

CREATE TABLE compaction_unwind (id serial PRIMARY KEY, body text);
CREATE INDEX compaction_unwind_idx ON compaction_unwind
    USING bm25(body) WITH (text_config = 'english');
SET pg_textsearch.segments_per_level = 2;

-- An error raised anywhere in a compaction pass must unwind without
-- losing documents or wedging the index: the pass is discarded, the
-- published graph still answers queries, and a later pass succeeds.
-- A buffer pin or index lock escaping the error path surfaces here as
-- a WARNING in the test output.
DO $$
DECLARE
    point text;
    n integer;
    docs integer;
    found integer;
    raised boolean;
BEGIN
    FOREACH point IN ARRAY ARRAY[
        'pg-textsearch-compaction-after-select',
        'pg-textsearch-compaction-alloc-output-data',
        'pg-textsearch-compaction-alloc-page-index',
        'pg-textsearch-before-compaction-publish',
        'pg-textsearch-compaction-after-restamp',
        'pg-textsearch-after-compaction-publish']
    LOOP
        -- Accumulate level 0 debt without letting the spill's own
        -- inline compaction consume it.
        PERFORM set_config(
            'pg_textsearch.segments_per_level', '64', true);
        FOR n IN 1..6 LOOP
            INSERT INTO compaction_unwind (body)
            SELECT format('unwind filler document %s %s', n, gs)
              FROM generate_series(1, 10) gs;
            PERFORM bm25_spill_index('compaction_unwind_idx');
        END LOOP;
        PERFORM set_config(
            'pg_textsearch.segments_per_level', '2', true);
        SELECT count(*) INTO docs FROM compaction_unwind;

        PERFORM injection_points_attach(point, 'error');
        raised := false;
        BEGIN
            PERFORM bm25_compact_step(
                        'compaction_unwind_idx'::regclass);
        EXCEPTION WHEN others THEN
            raised := true;
        END;
        PERFORM injection_points_detach(point);

        IF NOT raised THEN
            RAISE EXCEPTION 'injected error at % did not fire', point;
        END IF;

        SELECT count(*) INTO found FROM (
            SELECT 1
            FROM compaction_unwind
            ORDER BY body <@>
                     to_bm25query('filler', 'compaction_unwind_idx')
        ) ranked;
        IF found <> docs THEN
            RAISE EXCEPTION 'error at % lost documents: % of %',
                            point, found, docs;
        END IF;

        PERFORM bm25_compact('compaction_unwind_idx'::regclass);
        SELECT count(*) INTO found FROM (
            SELECT 1
            FROM compaction_unwind
            ORDER BY body <@>
                     to_bm25query('filler', 'compaction_unwind_idx')
        ) ranked;
        IF found <> docs THEN
            RAISE EXCEPTION 'compaction after % lost documents: % of %',
                            point, found, docs;
        END IF;
    END LOOP;
END
$$;

SELECT count(*) FROM compaction_unwind;
SELECT bm25_needs_compaction('compaction_unwind_idx'::regclass);

RESET pg_textsearch.segments_per_level;
DROP TABLE compaction_unwind CASCADE;
DROP EXTENSION injection_points;
DROP EXTENSION pg_textsearch CASCADE;
