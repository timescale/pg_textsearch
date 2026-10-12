-- NULL parameters must reach the AM, not be folded to a constant sort key.
\pset format unaligned
CREATE EXTENSION pg_textsearch;
CREATE TABLE null_orderby (id integer, content text, tags text[]);
INSERT INTO null_orderby VALUES
    (1, 'target', ARRAY['target']), (2, 'other', ARRAY['other']);
CREATE INDEX null_orderby_idx ON null_orderby USING bm25(content)
    WITH (text_config='english');
CREATE INDEX null_orderby_array_idx ON null_orderby USING bm25(tags)
    WITH (text_config='english');

SET enable_seqscan = off;
SET plan_cache_mode = force_generic_plan;
PREPARE null_query(bm25query) AS
    SELECT id FROM null_orderby ORDER BY content <@> $1 LIMIT 1;
EXPLAIN (COSTS OFF) EXECUTE null_query(NULL);
EXECUTE null_query(NULL);
-- The backend remains usable after rejecting the NULL scan key.
EXECUTE null_query('target');
DEALLOCATE null_query;

-- The text[] operator must reject NULL scan keys too.
PREPARE null_array(bm25query) AS
    SELECT id FROM null_orderby ORDER BY tags <@> $1 LIMIT 1;
EXPLAIN (COSTS OFF) EXECUTE null_array(NULL);
EXECUTE null_array(NULL);
EXECUTE null_array('target');
DEALLOCATE null_array;
RESET plan_cache_mode;
RESET enable_seqscan;
DROP TABLE null_orderby;
DROP EXTENSION pg_textsearch;
\pset format aligned
