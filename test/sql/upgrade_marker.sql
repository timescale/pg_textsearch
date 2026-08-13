-- Upgrade-marker warning tests (BUG-001 / BUG-002)
--
-- Reproduce the pre-v1.3 (metapage v6) on-disk state left behind when
-- an index still holds unspilled memtable / docid-chain data at the
-- moment its binary is swapped for a v1.3+ build, and verify the fix
-- surfaces a durable, client-visible WARNING that points at REINDEX --
-- on BOTH the read (scan) and the write (upgrade) paths -- and that a
-- REINDEX clears the marker so the warning stops.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

CREATE TABLE upg_docs (id SERIAL PRIMARY KEY, content TEXT);
CREATE INDEX upg_idx ON upg_docs USING bm25(content)
    WITH (text_config='english');

INSERT INTO upg_docs (content) VALUES
    ('alpha bravo charlie'),
    ('delta echo foxtrot'),
    ('golf hotel india');

-- Push everything to a segment so the on-disk memtable chain is empty,
-- then synthesize the legacy v6 metapage carrying a live docid pointer.
SELECT bm25_spill_index('upg_idx') > 0 AS spilled;
SELECT bm25_test_downgrade_metapage_v6('upg_idx', 1);

-- The durable marker is present on disk.
SELECT bm25_test_pending_docid('upg_idx') AS marker_after_downgrade;

-- READ PATH: a top-k scan must emit a client-visible WARNING.
SELECT count(*) FROM (
    SELECT 1 FROM upg_docs
    ORDER BY content <@> to_bm25query('alpha', 'upg_idx')
    LIMIT 5
) s;

-- Per-session throttle: the same session must NOT re-warn on a second
-- scan of the same index (avoids flooding nested-loop plans).
SELECT count(*) FROM (
    SELECT 1 FROM upg_docs
    ORDER BY content <@> to_bm25query('delta', 'upg_idx')
    LIMIT 5
) s;

-- WRITE PATH: the first write drives the v6->v8 upgrade, which must
-- also emit a client-visible WARNING and PRESERVE the marker in v8.
INSERT INTO upg_docs (content) VALUES ('juliet kilo lima');

-- Upgrade happened, yet the marker persists (durable until REINDEX).
SELECT bm25_test_pending_docid('upg_idx') AS marker_after_upgrade;

-- REINDEX rebuilds from the heap and clears the marker.
REINDEX INDEX upg_idx;
SELECT bm25_test_pending_docid('upg_idx') AS marker_after_reindex;

-- A scan after REINDEX is clean: no WARNING.
SELECT count(*) FROM (
    SELECT 1 FROM upg_docs
    ORDER BY content <@> to_bm25query('juliet', 'upg_idx')
    LIMIT 5
) s;

DROP TABLE upg_docs;
DROP EXTENSION IF EXISTS pg_textsearch CASCADE;
