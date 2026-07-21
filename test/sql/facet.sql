-- Faceted-search filter pushdown for BM25 (column OP constant + ORDER BY <@>)
--
-- The pushdown constrains Block-Max WAND to rows matching a scalar filter so
-- the top-k reflects only matching documents. Correctness must match the
-- standard post-filter path; PostgreSQL's Filter node above the index scan is
-- the exact recheck.

SET log_duration = off;
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET client_min_messages = NOTICE;
-- Force the BM25 index to satisfy the ORDER BY so the facet becomes a Filter.
SET enable_seqscan = false;

CREATE TABLE facet_docs (
    id       SERIAL PRIMARY KEY,
    category TEXT,
    rank     INT,
    content  TEXT
);

-- Many "common" docs and a few selective categories.
INSERT INTO facet_docs (category, rank, content)
SELECT 'common', g,
       'database query optimization common document number ' || g
FROM generate_series(1, 60) g;

INSERT INTO facet_docs (category, rank, content) VALUES
    ('engineering', 101, 'postgresql database indexing and query optimization engine'),
    ('engineering', 102, 'distributed database systems with storage and indexing'),
    ('engineering', 103, 'query optimization and database performance tuning'),
    ('engineering', 104, 'machine learning over a vector database for search'),
    ('support',     201, 'troubleshooting database connection and query errors'),
    ('support',     202, 'database backup and recovery support procedures');

CREATE INDEX facet_docs_idx ON facet_docs USING bm25(content)
    WITH (text_config='english');

ANALYZE facet_docs;

-- Plan keeps the facet as a Filter above the BM25 index scan.
EXPLAIN (COSTS OFF)
SELECT id, category
FROM facet_docs
WHERE category = 'engineering'
ORDER BY content <@> 'database query'
LIMIT 3;

-- Force the selectivity gate open so the pushdown path is exercised regardless
-- of planner row estimates, then confirm correctness.
SET pg_textsearch.facet_selectivity_threshold = 1.0;

SET pg_textsearch.enable_facet_pushdown = true;
SELECT id, category, ROUND((content <@> 'database query')::numeric, 4) AS score
FROM facet_docs
WHERE category = 'engineering'
ORDER BY content <@> 'database query', id
LIMIT 3;

-- Disabling pushdown must yield identical results (post-filter path).
SET pg_textsearch.enable_facet_pushdown = false;
SELECT id, category, ROUND((content <@> 'database query')::numeric, 4) AS score
FROM facet_docs
WHERE category = 'engineering'
ORDER BY content <@> 'database query', id
LIMIT 3;

SET pg_textsearch.enable_facet_pushdown = true;

-- Non-equality operator on a numeric column is supported too.
SELECT id, category, rank
FROM facet_docs
WHERE rank >= 201
ORDER BY content <@> 'database recovery', id
LIMIT 5;

-- A facet matching no rows yields no results.
SELECT id
FROM facet_docs
WHERE category = 'nonexistent'
ORDER BY content <@> 'database'
LIMIT 5;

-- A LIMIT larger than the matching set returns exactly the matching rows.
SELECT count(*) AS engineering_hits
FROM (
    SELECT id
    FROM facet_docs
    WHERE category = 'engineering'
    ORDER BY content <@> 'database'
    LIMIT 100
) s;

-- Gate closed (threshold 0): a broad facet falls back to the post-filter path
-- and still returns correct results.
SET pg_textsearch.facet_selectivity_threshold = 0.0;
SELECT count(*) AS common_hits
FROM (
    SELECT id
    FROM facet_docs
    WHERE category = 'common'
    ORDER BY content <@> 'database'
    LIMIT 100
) s;

-- Index-backed allow-list: a btree on the facet column lets the pushdown build
-- its allow-list with an index scan (O(matching rows)) instead of a full heap
-- scan. Results are identical; only the build path (reported by log_facet)
-- changes. enable_bitmapscan is disabled so the plan keeps the BM25 index scan
-- (and thus the pushdown) rather than a bitmap scan on the new facet index.
SET pg_textsearch.facet_selectivity_threshold = 1.0;
SET enable_bitmapscan = false;
CREATE INDEX facet_docs_category_idx ON facet_docs (category);
SET pg_textsearch.log_facet = on;
SELECT id, category
FROM facet_docs
WHERE category = 'engineering'
ORDER BY content <@> 'database query', id
LIMIT 3;
-- No index on rank: the pushdown falls back to the heap scan, still correct.
SELECT id, rank
FROM facet_docs
WHERE rank >= 201
ORDER BY content <@> 'database recovery', id
LIMIT 5;
SET pg_textsearch.log_facet = off;
RESET enable_bitmapscan;

RESET pg_textsearch.facet_selectivity_threshold;
RESET pg_textsearch.enable_facet_pushdown;
DROP TABLE facet_docs;

-- HOT-update regression: the BM25 segment stores HOT-chain root CTIDs (as every
-- index does), so the facet allow-list must be mapped to those same roots. If it
-- used live-tuple CTIDs instead, any HOT update would shift the live tuple and
-- the pushdown would silently drop matching rows from the top-k. fillfactor=50
-- leaves room so the update to a non-indexed column stays on-page (a HOT update,
-- which does not touch the index).
CREATE TABLE facet_hot (
    id    INT PRIMARY KEY,
    facet INT,
    note  TEXT,
    body  TEXT
) WITH (fillfactor = 50);

INSERT INTO facet_hot VALUES
    (1, 0, 'x', 'alpha beta'),
    (2, 0, 'x', 'alpha gamma delta'),
    (3, 0, 'x', 'alpha alpha alpha alpha'),
    (4, 0, 'x', 'alpha'),
    (5, 0, 'x', 'alpha epsilon');

CREATE INDEX facet_hot_idx ON facet_hot USING bm25(body)
    WITH (text_config='english');
ANALYZE facet_hot;

-- HOT update: changes only the non-indexed "note" column.
UPDATE facet_hot SET note = 'y' WHERE id = 3;

-- With the pushdown engaged, every matching row (including the HOT-updated id 3)
-- must still be returned, identical to the post-filter path.
SET pg_textsearch.facet_selectivity_threshold = 1.0;
SET pg_textsearch.enable_facet_pushdown = true;
SELECT id
FROM facet_hot
WHERE facet < 1
ORDER BY body <@> to_bm25query('alpha', 'facet_hot_idx'), id
LIMIT 5;

SET pg_textsearch.enable_facet_pushdown = false;
SELECT id
FROM facet_hot
WHERE facet < 1
ORDER BY body <@> to_bm25query('alpha', 'facet_hot_idx'), id
LIMIT 5;

-- Same rows with a btree on the facet column: the index path builds the
-- allow-list from index entries, which reference HOT-chain roots (as the BM25
-- segment does), so the HOT-updated id 3 is still returned. No HOT remap is
-- needed on this path.
SET enable_bitmapscan = false;
CREATE INDEX facet_hot_facet_idx ON facet_hot (facet);
SET pg_textsearch.log_facet = on;
SET pg_textsearch.enable_facet_pushdown = true;
SELECT id
FROM facet_hot
WHERE facet < 1
ORDER BY body <@> to_bm25query('alpha', 'facet_hot_idx'), id
LIMIT 5;
SET pg_textsearch.log_facet = off;
RESET enable_bitmapscan;

RESET pg_textsearch.facet_selectivity_threshold;
RESET pg_textsearch.enable_facet_pushdown;
DROP TABLE facet_hot;

-- Cross-type facet operator via a non-btree index. float4 = float8 is a
-- cross-type member of the hash opfamily, so the hash index is selected. The
-- scan-key subtype must be the operator's right-hand type; with the wrong
-- subtype the hash index probes the wrong bucket and returns an empty
-- allow-list, silently dropping every matching row. Enough rows are used that
-- the planner keeps the BM25 index scan (engaging the pushdown) over a hash
-- scan + sort. All rows match, so the correct answer is a full LIMIT.
SET enable_bitmapscan = false;
CREATE TABLE facet_xtype (id INT PRIMARY KEY, sc float4, content TEXT);
INSERT INTO facet_xtype
SELECT g, 2.5::float4, 'alpha document number ' || g
FROM generate_series(1, 2000) g;
CREATE INDEX facet_xtype_hash ON facet_xtype USING hash (sc);
CREATE INDEX facet_xtype_bm25 ON facet_xtype USING bm25(content)
    WITH (text_config='english');
ANALYZE facet_xtype;
SET pg_textsearch.facet_selectivity_threshold = 1.0;
SET pg_textsearch.log_facet = on;
SELECT count(*) AS xtype_hits FROM (
    SELECT id FROM facet_xtype
    WHERE sc = float8 '2.5'
    ORDER BY content <@> to_bm25query('alpha', 'facet_xtype_bm25')
    LIMIT 20
) s;
SET pg_textsearch.log_facet = off;
RESET pg_textsearch.facet_selectivity_threshold;
RESET enable_bitmapscan;
DROP TABLE facet_xtype;

-- Security: the pushdown must not build its allow-list across a Row-Level
-- Security boundary -- doing so would evaluate the facet predicate on, and leak
-- the count of, rows the caller cannot see. Under RLS it disengages (no facet
-- pushdown NOTICE below) and the post-filter path returns only the visible rows.
CREATE TABLE facet_rls (id INT PRIMARY KEY, tenant TEXT, facet INT, content TEXT);
INSERT INTO facet_rls
SELECT g, CASE WHEN g <= 5 THEN 'A' ELSE 'B' END, 0, 'alpha document ' || g
FROM generate_series(1, 40) g;
CREATE INDEX facet_rls_facet ON facet_rls (facet);
CREATE INDEX facet_rls_bm25 ON facet_rls USING bm25(content)
    WITH (text_config='english');
ANALYZE facet_rls;
ALTER TABLE facet_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY facet_rls_pol ON facet_rls USING (tenant = 'A');
CREATE ROLE facet_rls_reader;
GRANT SELECT ON facet_rls TO facet_rls_reader;
SET pg_textsearch.facet_selectivity_threshold = 1.0;
SET pg_textsearch.log_facet = on;
SET ROLE facet_rls_reader;
SELECT count(*) AS visible_hits FROM (
    SELECT id FROM facet_rls
    WHERE facet < 1
    ORDER BY content <@> to_bm25query('alpha', 'facet_rls_bm25')
    LIMIT 20
) s;
RESET ROLE;
SET pg_textsearch.log_facet = off;
RESET pg_textsearch.facet_selectivity_threshold;
DROP TABLE facet_rls;
DROP ROLE facet_rls_reader;

-- Stale-spec isolation: a faceted query whose BM25 index path is costed but not
-- executed (a seqscan wins) stashes a facet spec that is never consumed. That
-- spec must not leak into a later statement's scan on the same index. Both run
-- in one transaction so the transaction-end cleanup does not mask the leak.
CREATE TABLE facet_stale (id INT PRIMARY KEY, cat TEXT, content TEXT);
INSERT INTO facet_stale
SELECT g, CASE WHEN g <= 5 THEN 'x' ELSE 'y' END, 'alpha document ' || g
FROM generate_series(1, 20) g;
CREATE INDEX facet_stale_bm25 ON facet_stale USING bm25(content)
    WITH (text_config='english');
ANALYZE facet_stale;
BEGIN;
SET LOCAL pg_textsearch.facet_selectivity_threshold = 1.0;
-- A: enable_indexscan=off forces a seqscan, so the costed BM25 path's cat='x'
-- spec is stashed but never consumed. A itself is correct (Filter cat='x').
SET LOCAL enable_indexscan = off;
SELECT id FROM facet_stale WHERE cat = 'x'
    ORDER BY content <@> to_bm25query('alpha', 'facet_stale_bm25'), id
    LIMIT 5;
-- B: no facet, forced onto the BM25 scan. Must return ALL 20 rows, not the 5
-- that A's stale cat='x' spec would wrongly restrict it to.
SET LOCAL enable_indexscan = on;
SET LOCAL enable_seqscan = off;
SELECT count(*) AS b_all FROM (
    SELECT id FROM facet_stale
    ORDER BY content <@> to_bm25query('alpha', 'facet_stale_bm25')
    LIMIT 100
) s;
COMMIT;
DROP TABLE facet_stale;

-- Layered engagement: the pushdown must apply across BOTH on-disk segments and
-- the in-memory memtable, matching the post-filter path in every layer. Build
-- an index, spill twice to force multiple segments, then INSERT more rows that
-- stay in the memtable, so one faceted scan spans segments + memtable.
SET enable_bitmapscan = false;
CREATE TABLE facet_layers (id INT PRIMARY KEY, facet INT, content TEXT);
INSERT INTO facet_layers
SELECT g, g % 5, 'alpha beta gamma ' || g FROM generate_series(1, 100) g;
CREATE INDEX facet_layers_facet ON facet_layers (facet);
CREATE INDEX facet_layers_bm25 ON facet_layers USING bm25(content)
    WITH (text_config='english');
SELECT bm25_spill_index('facet_layers_bm25') IS NOT NULL AS spill1;
INSERT INTO facet_layers
SELECT g, g % 5, 'alpha beta delta ' || g FROM generate_series(101, 200) g;
SELECT bm25_spill_index('facet_layers_bm25') IS NOT NULL AS spill2;
-- Batch 3 stays in the memtable (no spill).
INSERT INTO facet_layers
SELECT g, g % 5, 'alpha epsilon zeta ' || g FROM generate_series(201, 300) g;
ANALYZE facet_layers;

SET pg_textsearch.facet_selectivity_threshold = 1.0;
SET pg_textsearch.log_facet = on;
-- Pushdown engages (NOTICE) and spans segments + memtable.
SELECT count(*) AS layers_on_hits FROM (
    SELECT id FROM facet_layers WHERE facet < 1
    ORDER BY content <@> to_bm25query('alpha', 'facet_layers_bm25') LIMIT 100
) s;
SET pg_textsearch.log_facet = off;

-- Parity across all layers: pushdown on vs off must be byte-identical.
DO $$
DECLARE on_ids int[]; off_ids int[];
BEGIN
    PERFORM set_config('pg_textsearch.enable_facet_pushdown', 'on', true);
    PERFORM set_config('pg_textsearch.facet_selectivity_threshold', '1.0', true);
    SELECT array_agg(id ORDER BY id) INTO on_ids FROM (
        SELECT id FROM facet_layers WHERE facet < 1
        ORDER BY content <@> to_bm25query('alpha', 'facet_layers_bm25'), id
        LIMIT 100) s;
    PERFORM set_config('pg_textsearch.enable_facet_pushdown', 'off', true);
    SELECT array_agg(id ORDER BY id) INTO off_ids FROM (
        SELECT id FROM facet_layers WHERE facet < 1
        ORDER BY content <@> to_bm25query('alpha', 'facet_layers_bm25'), id
        LIMIT 100) s;
    IF on_ids IS DISTINCT FROM off_ids THEN
        RAISE EXCEPTION 'facet_layers parity mismatch';
    END IF;
    RAISE NOTICE 'facet_layers parity OK (% rows over segments+memtable)',
        cardinality(on_ids);
END $$;

-- Two BM25 scans on the SAME index with DIFFERENT facets in ONE statement: each
-- must get its own facet, not the other's. The ExecutorRun sanitizer disables
-- the ambiguous pushdown so both fall to the correct post-filter path; before
-- it, whichever scan rescanned first stole the single stashed spec and the
-- other silently returned wrong rows.
SET pg_textsearch.enable_facet_pushdown = on;
SET pg_textsearch.facet_selectivity_threshold = 1.0;
SELECT
    (SELECT count(*) FROM (SELECT id FROM facet_layers WHERE facet < 1
        ORDER BY content <@> to_bm25query('alpha', 'facet_layers_bm25')
        LIMIT 100) a) AS two_scan_lt1,
    (SELECT count(*) FROM (SELECT id FROM facet_layers WHERE facet >= 4
        ORDER BY content <@> to_bm25query('alpha', 'facet_layers_bm25')
        LIMIT 100) b) AS two_scan_ge4;

-- Concurrent cursors on the same index: a no-facet cursor must not be confined
-- by a faceted cursor opened alongside it (the spec is bound to neither at the
-- AM, so the sanitizer clears it for the interleaved case).
DO $$
DECLARE c1 refcursor; c2 refcursor; r record; maxf int := -1; n int := 0;
BEGIN
    PERFORM set_config('pg_textsearch.enable_facet_pushdown', 'on', true);
    PERFORM set_config('pg_textsearch.facet_selectivity_threshold', '1.0', true);
    OPEN c1 FOR SELECT facet FROM facet_layers
        ORDER BY content <@> to_bm25query('alpha', 'facet_layers_bm25') LIMIT 100;
    OPEN c2 FOR SELECT id FROM facet_layers WHERE facet < 1
        ORDER BY content <@> to_bm25query('alpha', 'facet_layers_bm25') LIMIT 100;
    LOOP
        FETCH c1 INTO r; EXIT WHEN NOT FOUND;
        n := n + 1;
        IF r.facet > maxf THEN maxf := r.facet; END IF;
    END LOOP;
    CLOSE c1; CLOSE c2;
    IF maxf < 4 THEN
        RAISE EXCEPTION 'cursor facet leak: c1 confined to facet <= % (n=%)',
            maxf, n;
    END IF;
    RAISE NOTICE 'cursor isolation OK (c1 spans facet 0..%, n=%)', maxf, n;
END $$;
RESET enable_bitmapscan;
RESET pg_textsearch.facet_selectivity_threshold;
RESET pg_textsearch.enable_facet_pushdown;
DROP TABLE facet_layers;

DROP EXTENSION pg_textsearch CASCADE;
