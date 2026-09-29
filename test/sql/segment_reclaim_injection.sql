-- Deferred reclaim asserted with a pinned horizon.
--
-- segment_reclaim.sql cannot assert that VACUUM parks a dropped
-- segment's pages, because a concurrently advancing xid horizon may let
-- the same VACUUM reclaim them immediately.  Pin the horizon with an
-- injection point so parking is observable, then release it and confirm
-- a later VACUUM drains the parked pages.
\pset format unaligned
SET client_min_messages = warning;
CREATE EXTENSION pg_textsearch;
CREATE EXTENSION injection_points;
CREATE EXTENSION pg_textsearch_test;

CREATE TABLE reclaim_hold_docs (id int, body text)
    WITH (autovacuum_enabled = false);
INSERT INTO reclaim_hold_docs
SELECT g, 'alpha beta gamma term' || (g % 50)
FROM generate_series(1, 1500) g;
CREATE INDEX reclaim_hold_idx ON reclaim_hold_docs
    USING bm25 (body)
    WITH (text_config = 'english', compaction = 'off');

-- Second segment holding only rows that are about to be deleted, so
-- VACUUM drops it wholesale and displaces its pages.
INSERT INTO reclaim_hold_docs
SELECT g, 'delta epsilon term' || (g % 50)
FROM generate_series(3001, 3400) g;
SELECT bm25_spill_index('reclaim_hold_idx') > 0 AS second_segment_spilled;

SELECT bm25_pending_free_pages('reclaim_hold_idx') AS parked_before_vacuum;

SELECT pg_textsearch_test_attach_reclaim_horizon_hold();

DELETE FROM reclaim_hold_docs WHERE id BETWEEN 3001 AND 3400;
VACUUM reclaim_hold_docs;

-- The horizon is pinned, so the dropped segment's pages must still be
-- parked rather than recycled.
SELECT bm25_pending_free_pages('reclaim_hold_idx') > 0
    AS parked_after_vacuum_drop;

-- Repeat VACUUMs must not drain them while the horizon stays pinned.
VACUUM reclaim_hold_docs;
SELECT bm25_pending_free_pages('reclaim_hold_idx') > 0
    AS still_parked_after_second_vacuum;

-- Release the horizon; the next VACUUM drains every parked page.
SELECT injection_points_detach('pg-textsearch-reclaim-horizon');
SELECT txid_current() IS NOT NULL AS t1;
SELECT txid_current() IS NOT NULL AS t2;
VACUUM reclaim_hold_docs;
SELECT bm25_pending_free_pages('reclaim_hold_idx') AS parked_after_release;

-- Queries still work after reclaim.
SELECT count(*) > 0 AS has_hits
FROM (
    SELECT 1 FROM reclaim_hold_docs
    ORDER BY body <@> to_bm25query('alpha', 'reclaim_hold_idx')
    LIMIT 10
) s;

DROP TABLE reclaim_hold_docs;
DROP EXTENSION pg_textsearch_test;
DROP EXTENSION injection_points;
DROP EXTENSION pg_textsearch;
\pset format aligned
