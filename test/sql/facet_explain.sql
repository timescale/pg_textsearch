-- Faceted-search pushdown: EXPLAIN observability.
--
-- On PostgreSQL 18+ the BM25 index scan reports, via the per-node EXPLAIN
-- hook, how the facet allow-list was built ("via index" / "via heap scan").
-- The line appears only under EXPLAIN ANALYZE and only when the pushdown
-- engaged, so its presence distinguishes a facet-index-backed pushdown from a
-- plain BM25 scan whose facet was left to the post-filter. PostgreSQL 17 has
-- no per-node EXPLAIN hook, so the line never appears there (log_facet reports
-- the same information); the PG17 output is the facet_explain_1.out variant.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET client_min_messages = warning;
SET enable_seqscan = off;
SET enable_bitmapscan = off;

CREATE TABLE fe (id INT PRIMARY KEY, facet INT, content TEXT);
INSERT INTO fe
SELECT g, g % 10, 'alpha beta gamma ' || g FROM generate_series(1, 200) g;
CREATE INDEX fe_facet ON fe (facet);
CREATE INDEX fe_bm25 ON fe USING bm25(content) WITH (text_config='english');
ANALYZE fe;
SET pg_textsearch.facet_selectivity_threshold = 1.0;

-- Extract just the pushdown annotation line (stable across versions and free
-- of volatile EXPLAIN ANALYZE fields), or 'none' when it is absent.
CREATE FUNCTION fe_pushdown(q text) RETURNS text AS $$
DECLARE ln text; res text := 'none';
BEGIN
    FOR ln IN EXECUTE
        'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) '
        || q
    LOOP
        IF position('BM25 Facet Pushdown:' IN ln) > 0 THEN
            res := btrim(ln);
        END IF;
    END LOOP;
    RETURN res;
END $$ LANGUAGE plpgsql;

-- (a) facet index present: allow-list built via the facet index. facet < 1 is
-- facet 0 = 20 of the 200 rows.
SELECT fe_pushdown($q$
    SELECT id FROM fe WHERE facet < 1
    ORDER BY content <@> to_bm25query('alpha', 'fe_bm25') LIMIT 5$q$)
    AS via_index;

-- (b) pushdown disabled: no annotation (the facet is a plain post-filter).
SET pg_textsearch.enable_facet_pushdown = off;
SELECT fe_pushdown($q$
    SELECT id FROM fe WHERE facet < 1
    ORDER BY content <@> to_bm25query('alpha', 'fe_bm25') LIMIT 5$q$)
    AS disabled;
SET pg_textsearch.enable_facet_pushdown = on;

-- (c) no index on the facet column: allow-list built via a heap scan.
DROP INDEX fe_facet;
SELECT fe_pushdown($q$
    SELECT id FROM fe WHERE facet < 1
    ORDER BY content <@> to_bm25query('alpha', 'fe_bm25') LIMIT 5$q$)
    AS via_heap;

DROP FUNCTION fe_pushdown(text);
DROP TABLE fe;
DROP EXTENSION pg_textsearch CASCADE;
