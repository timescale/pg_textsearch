-- Invalid head/next links must use corruption handling, not fail in I/O.
\pset format unaligned
SET client_min_messages = error;
CREATE EXTENSION pg_textsearch;
CREATE EXTENSION injection_points;
CREATE EXTENSION pg_textsearch_test;
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 0;

-- Keep the valid prefix parked so recovery must unlink its bad next link.
SELECT pg_textsearch_test_attach_reclaim_horizon_hold();
CREATE TEMP TABLE expected_pending (n bigint);

CREATE FUNCTION pg_temp.corrupt_link(at_head boolean, target text)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    parked bigint;
    bad_block bigint;
BEGIN
    CREATE TEMP TABLE bounds_docs (id int, body text);
    INSERT INTO bounds_docs
        SELECT g, 'alpha beta' FROM generate_series(1, 40) g;
    CREATE INDEX bounds_idx ON bounds_docs USING bm25(body)
        WITH (text_config = 'simple', compaction = 'manual');
    INSERT INTO bounds_docs
        SELECT g, 'alpha gamma' FROM generate_series(41, 80) g;
    PERFORM bm25_spill_index('bounds_idx');
    PERFORM bm25_force_merge('bounds_idx');
    parked := bm25_pending_free_pages('bounds_idx');
    IF parked <= 0 THEN
        RAISE EXCEPTION 'expected parked source pages';
    END IF;
    INSERT INTO expected_pending
        VALUES (CASE WHEN at_head THEN 0 ELSE parked END);
    bad_block := CASE target
        WHEN 'zero' THEN 0
        WHEN 'eof' THEN pg_relation_size('bounds_idx') /
            current_setting('block_size')::bigint
        WHEN 'past_eof' THEN pg_relation_size('bounds_idx') /
            current_setting('block_size')::bigint + 100
        ELSE NULL
    END;
    IF bad_block IS NULL THEN
        RAISE EXCEPTION 'unknown corruption target';
    END IF;
    PERFORM pg_textsearch_test_set_tombstone_link(
        'bounds_idx', at_head, bad_block);
END
$$;

CREATE FUNCTION pg_temp.check_diagnostic()
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    -- Repeating the diagnostic must still error: it cannot repair the chain.
    FOR attempt IN 1..2 LOOP
        BEGIN
            PERFORM bm25_pending_free_pages('bounds_idx');
            RAISE EXCEPTION 'corrupt link was accepted';
        EXCEPTION WHEN data_corrupted THEN
            IF SQLERRM NOT LIKE 'pg_textsearch: corrupt tombstone page %' THEN
                RAISE;
            END IF;
        END;
    END LOOP;
END
$$;

CREATE FUNCTION pg_temp.check_recovery()
RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    hits int[];
BEGIN
    IF bm25_pending_free_pages('bounds_idx') <>
        (SELECT n FROM expected_pending) THEN
        RAISE EXCEPTION 'recovery lost the valid prefix or kept the bad link';
    END IF;
    INSERT INTO bounds_docs VALUES (81, 'probe');
    SELECT array_agg(id) INTO hits FROM (
        SELECT id FROM bounds_docs
        ORDER BY body <@> to_bm25query('probe', 'bounds_idx') LIMIT 1
    ) ranked;
    IF hits IS DISTINCT FROM ARRAY[81] THEN
        RAISE EXCEPTION 'index unusable after recovery: %', hits;
    END IF;
    DROP TABLE bounds_docs;
    TRUNCATE expected_pending;
END
$$;

SELECT pg_temp.corrupt_link(true, 'zero');
SELECT pg_temp.check_diagnostic();
VACUUM bounds_docs;
SELECT pg_temp.check_recovery();

SELECT pg_temp.corrupt_link(true, 'eof');
SELECT pg_temp.check_diagnostic();
VACUUM bounds_docs;
SELECT pg_temp.check_recovery();

SELECT pg_temp.corrupt_link(true, 'past_eof');
SELECT pg_temp.check_diagnostic();
VACUUM bounds_docs;
SELECT pg_temp.check_recovery();

SELECT pg_temp.corrupt_link(false, 'zero');
SELECT pg_temp.check_diagnostic();
VACUUM bounds_docs;
SELECT pg_temp.check_recovery();

SELECT pg_temp.corrupt_link(false, 'eof');
SELECT pg_temp.check_diagnostic();
VACUUM bounds_docs;
SELECT pg_temp.check_recovery();

SELECT pg_temp.corrupt_link(false, 'past_eof');
SELECT pg_temp.check_diagnostic();
VACUUM bounds_docs;
SELECT pg_temp.check_recovery();

SELECT injection_points_detach('pg-textsearch-reclaim-horizon');
DROP TABLE expected_pending;
DROP EXTENSION pg_textsearch_test;
DROP EXTENSION injection_points;
DROP EXTENSION pg_textsearch;
