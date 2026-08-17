-- Read-path coverage for the STANDALONE scoring path.
--
-- The client-visible "results may be incomplete" WARNING for a
-- pre-v1.3 upgraded index must also reach workloads that use
-- standalone scoring -- text <@> bm25query evaluated per row (e.g. as
-- a projection) -- not only ORDER BY ... LIMIT index scans, which go
-- through tp_beginscan.  This runs in its own regression file so the
-- per-session warning throttle starts clean.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

CREATE TABLE upg_sa_docs (id SERIAL PRIMARY KEY, content TEXT);
CREATE INDEX upg_sa_idx ON upg_sa_docs USING bm25(content)
    WITH (text_config='english');

INSERT INTO upg_sa_docs (content) VALUES
    ('alpha bravo charlie'),
    ('delta echo foxtrot'),
    ('golf hotel india');

-- Spill so the on-disk memtable chain is empty, then synthesize the
-- legacy v6 metapage carrying a live docid pointer.
SELECT bm25_spill_index('upg_sa_idx') > 0 AS spilled;
SELECT bm25_test_downgrade_metapage_v6('upg_sa_idx', 1);

-- STANDALONE scoring: a WHERE clause with <@> evaluates the operator
-- per row as a scalar function (a seq scan, NOT an index scan), so it
-- does not go through tp_beginscan.  This deliberately exercises the
-- standalone scoring code path as a test target (cf. validation.sql,
-- queries.sql).  It must still emit the client-visible WARNING.
SELECT count(*) FROM upg_sa_docs
    WHERE content <@> to_bm25query('alpha', 'upg_sa_idx') < 0;

DROP TABLE upg_sa_docs;
DROP EXTENSION IF EXISTS pg_textsearch CASCADE;
