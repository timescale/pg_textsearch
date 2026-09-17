CREATE EXTENSION pg_textsearch;

SET default_text_search_config = 'pg_catalog.english';

CREATE TABLE boolean_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO boolean_docs VALUES
    (1, 'refund approved'),
    (2, 'refund fraud'),
    (3, 'fraud refund investigation'),
    (4, 'billing refund request'),
    (5, 'billing question'),
    (8, ''),
    (9, 'the');

SET client_min_messages = WARNING;
CREATE INDEX boolean_docs_body_idx ON boolean_docs USING bm25(body)
    WITH (text_config = 'english');
RESET client_min_messages;

INSERT INTO boolean_docs VALUES
    (6, 'refund fraud followup'),
    (7, 'refund suspected fraud');

SET enable_seqscan = off;

SET pg_textsearch.memtable_cache_enabled = off;
SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', '!fraud')
ORDER BY id;
RESET pg_textsearch.memtable_cache_enabled;

EXPLAIN (COSTS OFF)
SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund & fraud');

CREATE FUNCTION pg_temp.first_plan_child(query text)
RETURNS text
LANGUAGE plpgsql
AS $$
DECLARE
    plan json;
BEGIN
    EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) ' || query INTO plan;
    RETURN plan->0->'Plan'->'Plans'->0->>'Node Type';
END
$$;

CREATE TEMP TABLE boolean_other (id integer);
INSERT INTO boolean_other SELECT generate_series(1, 9);

\pset format unaligned
SET enable_sort = off;
SELECT pg_temp.first_plan_child($query$
    SELECT id
    FROM boolean_docs
    WHERE body @@ to_tsquery('english', 'refund')
    ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
    LIMIT 1
$query$) = 'Index Scan' AS combined_path_uses_index;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'billing & refund')
ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
LIMIT 1;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund & !fraud')
ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
LIMIT 2;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'bill:*')
ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
LIMIT 2;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'billing <-> refund')
ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
LIMIT 2;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'billing & !fraud')
ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
LIMIT 2;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund & mysql')
ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
LIMIT 2;

SET enable_seqscan = on;
SET enable_sort = on;
SELECT pg_temp.first_plan_child($query$
    SELECT id
    FROM boolean_docs
    WHERE body @@ to_tsquery('english', '!fraud')
    ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
    LIMIT 1
$query$) = 'Sort' AS combined_negative_full_scan_falls_back;

SELECT pg_temp.first_plan_child($query$
    SELECT id
    FROM boolean_docs
    WHERE body @@ to_tsquery('english', 'bill:*')
    ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
    LIMIT 1
$query$) = 'Sort' AS combined_prefix_full_scan_falls_back;

SELECT pg_temp.first_plan_child($query$
    SELECT id
    FROM boolean_docs
    WHERE body @@ to_tsquery('english', 'refund:A')
    ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
    LIMIT 1
$query$) = 'Sort' AS combined_weight_full_scan_falls_back;

SET plan_cache_mode = force_generic_plan;
PREPARE combined_full_scan_cost(tsquery) AS
SELECT id
FROM boolean_docs
WHERE body @@ $1
ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
LIMIT 1;
SELECT pg_temp.first_plan_child(
    'EXECUTE combined_full_scan_cost(to_tsquery(''english'', ''refund''))'
) = 'Sort' AS combined_parameter_full_scan_falls_back;
DEALLOCATE combined_full_scan_cost;
RESET plan_cache_mode;

SET enable_seqscan = off;
SET enable_sort = off;

SET plan_cache_mode = force_generic_plan;
PREPARE combined_ranked_scan(tsquery) AS
SELECT id
FROM boolean_docs
WHERE body @@ $1
ORDER BY body <@> to_bm25query('refund', 'boolean_docs_body_idx')
LIMIT 2;

EXECUTE combined_ranked_scan(to_tsquery('english', 'refund & !fraud'));
EXECUTE combined_ranked_scan(NULL);
SET client_min_messages = WARNING;
EXECUTE combined_ranked_scan(to_tsquery('english', ''));
RESET client_min_messages;
DEALLOCATE combined_ranked_scan;
RESET plan_cache_mode;

CREATE TABLE boolean_low_work_mem_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO boolean_low_work_mem_docs
SELECT id, 'rank'
FROM generate_series(1, 5000) AS id;

INSERT INTO boolean_low_work_mem_docs
SELECT id, 'filter'
FROM generate_series(5001, 8000) AS id;

INSERT INTO boolean_low_work_mem_docs VALUES
    (8001, 'rank filter filler filler filler filler filler filler');

SET default_text_search_config = 'pg_catalog.simple';
SET work_mem = '64kB';
SET pg_textsearch.filtered_seed = off;
SET client_min_messages = WARNING;
CREATE INDEX boolean_low_work_mem_docs_body_idx
    ON boolean_low_work_mem_docs USING bm25(body)
    WITH (text_config = 'simple');
RESET client_min_messages;

SELECT id
FROM boolean_low_work_mem_docs
WHERE body @@ to_tsquery('simple', 'filter')
ORDER BY body <@> to_bm25query('rank', 'boolean_low_work_mem_docs_body_idx')
LIMIT 1;

DROP TABLE boolean_low_work_mem_docs;

CREATE TABLE boolean_rank_cap_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO boolean_rank_cap_docs
SELECT id, 'rank'
FROM generate_series(1, 100000) AS id;

INSERT INTO boolean_rank_cap_docs
SELECT id, 'filter'
FROM generate_series(100001, 103000) AS id;

INSERT INTO boolean_rank_cap_docs VALUES
    (103001, 'rank filter filler filler filler filler filler filler');

SET client_min_messages = WARNING;
CREATE INDEX boolean_rank_cap_docs_body_idx
    ON boolean_rank_cap_docs USING bm25(body)
    WITH (text_config = 'simple');
RESET client_min_messages;

SELECT pg_temp.first_plan_child($query$
    SELECT id
    FROM boolean_rank_cap_docs
    WHERE body @@ to_tsquery('simple', 'filter')
    ORDER BY body <@> to_bm25query('rank', 'boolean_rank_cap_docs_body_idx')
    LIMIT 1
$query$) = 'Sort' AS combined_rank_cap_falls_back;

SELECT id
FROM boolean_rank_cap_docs
WHERE body @@ to_tsquery('simple', 'filter')
ORDER BY body <@> to_bm25query('rank', 'boolean_rank_cap_docs_body_idx')
LIMIT 1;

RESET pg_textsearch.filtered_seed;
RESET work_mem;
SET default_text_search_config = 'pg_catalog.english';
DROP TABLE boolean_rank_cap_docs;

SET enable_nestloop = off;
SELECT pg_temp.first_plan_child($query$
    SELECT d.id
    FROM boolean_docs d
    JOIN boolean_other other ON other.id = d.id
    WHERE d.body @@ to_tsquery('english', 'refund')
      AND d.body @@ to_tsquery('english', 'fraud')
    ORDER BY d.body <@> to_bm25query('refund', 'boolean_docs_body_idx')
    LIMIT 1
$query$) = 'Sort' AS multi_key_join_path_falls_back;
RESET enable_nestloop;
RESET enable_sort;
\pset format aligned

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund & fraud')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund | fraud')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund & !fraud')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'billing & (refund | !question)')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund <-> fraud')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', '!fraud')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund | !fraud')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', '!(refund <-> fraud)')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund:*')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund:A')
ORDER BY id;

SET plan_cache_mode = force_generic_plan;
PREPARE boolean_scan(tsquery) AS
SELECT id
FROM boolean_docs
WHERE body @@ $1
ORDER BY id;

EXPLAIN (COSTS OFF)
EXECUTE boolean_scan(to_tsquery('english', 'refund & !fraud'));

EXECUTE boolean_scan(to_tsquery('english', 'refund & fraud'));
EXECUTE boolean_scan(to_tsquery('english', '!fraud'));
EXECUTE boolean_scan(to_tsquery('english', 'refund <-> fraud'));
EXECUTE boolean_scan(to_tsquery('english', 'refund:*'));

SET default_text_search_config = 'pg_catalog.simple';
EXECUTE boolean_scan(NULL);
EXECUTE boolean_scan(to_tsquery('simple', 'refund & fraud'));
DEALLOCATE boolean_scan;
RESET plan_cache_mode;

RESET enable_seqscan;
EXPLAIN (COSTS OFF)
SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('simple', 'refund & fraud');
SET enable_seqscan = off;

SET default_text_search_config = 'pg_catalog.english';
SET client_min_messages = WARNING;
SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', '')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'the')
ORDER BY id;
RESET client_min_messages;

UPDATE boolean_docs
SET body = 'billing fraud question'
WHERE id = 5;
DELETE FROM boolean_docs WHERE id = 1;
VACUUM boolean_docs;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', '!fraud')
ORDER BY id;

SELECT id
FROM boolean_docs
WHERE body @@ to_tsquery('english', 'refund & !fraud')
ORDER BY id;

RESET enable_seqscan;
RESET default_text_search_config;

DROP TABLE boolean_docs;

CREATE TABLE boolean_empty_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

SET client_min_messages = WARNING;
CREATE INDEX boolean_empty_docs_body_idx ON boolean_empty_docs USING bm25(body)
    WITH (text_config = 'english');
RESET client_min_messages;

INSERT INTO boolean_empty_docs VALUES
    (1, ''),
    (2, 'the');

SET enable_seqscan = off;
SET pg_textsearch.memtable_cache_enabled = off;
SELECT id
FROM boolean_empty_docs
WHERE body @@ to_tsquery('english', '!missing')
ORDER BY id;
RESET pg_textsearch.memtable_cache_enabled;

SELECT bm25_spill_index('boolean_empty_docs_body_idx') IS NOT NULL
    AS spilled_empty_docs;

INSERT INTO boolean_empty_docs VALUES
    (3, ''),
    (4, 'the');

SELECT bm25_spill_index('boolean_empty_docs_body_idx') IS NOT NULL
    AS spilled_more_empty_docs;
SELECT bm25_force_merge('boolean_empty_docs_body_idx');

SELECT id
FROM boolean_empty_docs
WHERE body @@ to_tsquery('english', '!missing')
ORDER BY id;

RESET enable_seqscan;
DROP TABLE boolean_empty_docs;

CREATE TABLE boolean_segment_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO boolean_segment_docs VALUES
    (1, 'alpha beta'),
    (2, 'alpha'),
    (3, 'beta');

SET client_min_messages = WARNING;
CREATE INDEX boolean_segment_docs_body_idx
    ON boolean_segment_docs USING bm25(body)
    WITH (text_config = 'english');
RESET client_min_messages;

INSERT INTO boolean_segment_docs VALUES
    (4, 'gamma delta'),
    (5, 'gamma'),
    (6, 'delta'),
    (7, 'alpha gamma');

SELECT bm25_spill_index('boolean_segment_docs_body_idx') IS NOT NULL
    AS spilled_segment_docs;

INSERT INTO boolean_segment_docs VALUES
    (8, 'alpha delta'),
    (9, 'beta gamma'),
    (10, 'epsilon');

DELETE FROM boolean_segment_docs WHERE id = 2;
VACUUM boolean_segment_docs;

SET enable_seqscan = off;

SELECT array_agg(id ORDER BY id) AS nested_matches
FROM boolean_segment_docs
WHERE body @@ to_tsquery('english', '(alpha & beta) | (gamma & delta)');

SELECT array_agg(id ORDER BY id) AS anchored_not_matches
FROM boolean_segment_docs
WHERE body @@ to_tsquery('english', 'alpha & !gamma');

SELECT array_agg(id ORDER BY id) AS duplicate_term_matches
FROM boolean_segment_docs
WHERE body @@ to_tsquery('english', 'alpha | alpha');

SELECT count(*) AS missing_and_matches
FROM boolean_segment_docs
WHERE body @@ to_tsquery('english', 'alpha & missing');

SELECT array_agg(id ORDER BY id) AS mixed_source_matches
FROM boolean_segment_docs
WHERE body @@ to_tsquery('english', 'alpha | epsilon');

SELECT array_agg(id ORDER BY id) AS pure_negative_matches
FROM boolean_segment_docs
WHERE body @@ to_tsquery('english', '!missing');

SELECT array_agg(id ORDER BY id) AS phrase_matches
FROM boolean_segment_docs
WHERE body @@ to_tsquery('english', 'alpha <-> beta');

RESET enable_seqscan;
DROP TABLE boolean_segment_docs;

SET default_text_search_config = 'pg_catalog.simple';

CREATE TABLE boolean_prefix_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO boolean_prefix_docs VALUES
    (1, 'cat'),
    (2, 'cater'),
    (3, 'dog');

SET client_min_messages = WARNING;
SET pg_textsearch.compress_segments = off;
CREATE INDEX boolean_prefix_docs_body_idx
    ON boolean_prefix_docs USING bm25(body)
    WITH (text_config = 'simple');
RESET client_min_messages;

INSERT INTO boolean_prefix_docs VALUES
    (4, 'cattle'),
    (5, 'catalog'),
    (6, 'catch');

\pset format unaligned
SELECT bm25_spill_index('boolean_prefix_docs_body_idx') IS NOT NULL
    AS spilled_prefix_docs;
RESET pg_textsearch.compress_segments;

INSERT INTO boolean_prefix_docs VALUES
    (7, 'catfish'),
    (8, 'category'),
    (9, 'bird'),
    (10, 'cat cater');

SET enable_seqscan = off;

SELECT array_agg(id ORDER BY id) AS prefix_matches
FROM boolean_prefix_docs
WHERE body @@ to_tsquery('simple', 'cat:*');
\pset format aligned

RESET enable_seqscan;
DROP TABLE boolean_prefix_docs;

CREATE TABLE boolean_broad_prefix_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO boolean_broad_prefix_docs
SELECT id,
       CASE WHEN id <= 257
           THEN format('broadprefix%s', lpad(id::text, 4, '0'))
           ELSE 'unrelated'
       END
FROM generate_series(1, 258) AS id;

SET client_min_messages = WARNING;
CREATE INDEX boolean_broad_prefix_docs_body_idx
    ON boolean_broad_prefix_docs USING bm25(body)
    WITH (text_config = 'simple');
RESET client_min_messages;

SET enable_seqscan = off;

\pset format unaligned
SELECT count(*) AS broad_prefix_matches
FROM boolean_broad_prefix_docs
WHERE body @@ to_tsquery('simple', 'broadprefix:*');
\pset format aligned

RESET enable_seqscan;
DROP TABLE boolean_broad_prefix_docs;

SET default_text_search_config = 'pg_catalog.english';

CREATE TABLE boolean_seek_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO boolean_seek_docs
SELECT id,
       concat_ws(' ',
           CASE WHEN id % 2 = 0 THEN 'common' END,
           CASE WHEN id % 997 = 0 OR id = 5001 THEN 'anchor' END)
FROM generate_series(1, 5001) AS id;

SET client_min_messages = WARNING;
CREATE INDEX boolean_seek_docs_body_idx
    ON boolean_seek_docs USING bm25(body)
    WITH (text_config = 'english');
RESET client_min_messages;

ANALYZE boolean_seek_docs;

\pset format unaligned
SELECT pg_temp.first_plan_child($query$
    SELECT id
    FROM boolean_seek_docs
    WHERE body @@ to_tsquery('english', 'common')
    LIMIT 1
$query$) = 'Seq Scan' AS boolean_limit_avoids_materializing_index;
\pset format aligned

SET enable_seqscan = off;

\pset format unaligned
SELECT array_agg(id ORDER BY id) AS galloping_seek_matches
FROM boolean_seek_docs
WHERE body @@ to_tsquery('english', 'anchor & common');
\pset format aligned

RESET enable_seqscan;
DROP TABLE boolean_seek_docs;

CREATE TABLE boolean_memtable_stream_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

SET client_min_messages = WARNING;
CREATE INDEX boolean_memtable_stream_docs_body_idx
    ON boolean_memtable_stream_docs USING bm25(body)
    WITH (text_config = 'english');
RESET client_min_messages;

SET pg_textsearch.bulk_load_threshold = 0;
SET pg_textsearch.memtable_pages_threshold = 0;

INSERT INTO boolean_memtable_stream_docs
SELECT id,
       concat_ws(' ',
           CASE WHEN id % 2 = 0 THEN 'common' END,
           CASE WHEN id % 997 = 0 OR id = 5001 THEN 'anchor' END,
           CASE WHEN id = 5001 THEN 'alternative' END,
           'ubiquitous')
FROM generate_series(1, 5001) AS id;

SET enable_seqscan = off;

\pset format unaligned
SELECT array_agg(id ORDER BY id) AS memtable_selective_and_matches
FROM boolean_memtable_stream_docs
WHERE body @@ to_tsquery('english', 'anchor & common');

SELECT array_agg(id ORDER BY id) AS memtable_nested_matches
FROM boolean_memtable_stream_docs
WHERE body @@ to_tsquery(
    'english', 'anchor & (common | alternative)');

SELECT array_agg(id ORDER BY id) AS memtable_anchored_not_matches
FROM boolean_memtable_stream_docs
WHERE body @@ to_tsquery('english', 'anchor & !common');

SELECT count(*) AS memtable_nested_not_matches
FROM boolean_memtable_stream_docs
WHERE body @@ to_tsquery(
    'english', '(alternative | !common) & ubiquitous');
\pset format aligned

SET pg_textsearch.memtable_cache_enabled = off;

\pset format unaligned
SELECT array_agg(id ORDER BY id) AS chain_selective_and_matches
FROM boolean_memtable_stream_docs
WHERE body @@ to_tsquery('english', 'anchor & common');

SELECT count(*) AS chain_nested_not_matches
FROM boolean_memtable_stream_docs
WHERE body @@ to_tsquery(
    'english', '(alternative | !common) & ubiquitous');
\pset format aligned

RESET pg_textsearch.memtable_cache_enabled;
RESET enable_seqscan;
RESET pg_textsearch.bulk_load_threshold;
RESET pg_textsearch.memtable_pages_threshold;
DROP TABLE boolean_memtable_stream_docs;

CREATE TABLE boolean_many_term_docs (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO boolean_many_term_docs
SELECT document, query.body
FROM generate_series(1, 1000) AS document
CROSS JOIN (
    SELECT string_agg(format('term%s', term), ' ') AS body
    FROM generate_series(1, 201) AS term
) AS query;

SET pg_textsearch.compress_segments = off;
SET client_min_messages = WARNING;
CREATE INDEX boolean_many_term_docs_body_idx
    ON boolean_many_term_docs USING bm25(body)
    WITH (text_config = 'simple');
RESET client_min_messages;
RESET pg_textsearch.compress_segments;

CREATE FUNCTION pg_temp.boolean_and_query(term_count integer)
RETURNS tsquery
LANGUAGE sql
IMMUTABLE
STRICT
AS $$
    SELECT string_agg(format('term%s', term), ' & ')::tsquery
    FROM generate_series(1, term_count) AS term
$$;

CREATE FUNCTION pg_temp.boolean_repeated_or_query(term_count integer)
RETURNS tsquery
LANGUAGE sql
IMMUTABLE
STRICT
AS $$
    SELECT string_agg('term1', ' | ')::tsquery
    FROM generate_series(1, term_count)
$$;

SET default_text_search_config = 'pg_catalog.simple';
SELECT pg_temp.boolean_and_query(64)::text AS query \gset exact_limit_
SELECT pg_temp.boolean_and_query(201)::text AS query \gset oversized_
SELECT pg_temp.boolean_repeated_or_query(65)::text AS query \gset repeated_

\pset format unaligned
SELECT pg_temp.first_plan_child(format(
    'SELECT count(*) FROM boolean_many_term_docs WHERE body @@ %L::tsquery',
    :'oversized_query'
)) = 'Seq Scan' AS oversized_boolean_falls_back;
SELECT pg_temp.first_plan_child(format(
    'SELECT count(*) FROM boolean_many_term_docs WHERE body @@ %L::tsquery',
    :'repeated_query'
)) = 'Seq Scan' AS repeated_boolean_falls_back;
SELECT pg_temp.first_plan_child(format(
    'SELECT id FROM boolean_many_term_docs WHERE body @@ %L::tsquery '
    'ORDER BY body <@> to_bm25query(''term1'', '
    '''boolean_many_term_docs_body_idx'') LIMIT 1',
    :'oversized_query'
)) = 'Sort' AS oversized_combined_boolean_falls_back;

SET enable_seqscan = off;
SELECT pg_temp.first_plan_child(format(
    'SELECT count(*) FROM boolean_many_term_docs WHERE body @@ %L::tsquery',
    :'exact_limit_query'
)) = 'Index Scan' AS exact_operand_limit_uses_index;
SELECT count(*) = 1000 AS exact_operand_limit_succeeds
FROM boolean_many_term_docs
WHERE body @@ :'exact_limit_query'::tsquery;
RESET enable_seqscan;
\pset format aligned
DROP TABLE boolean_many_term_docs;

DROP EXTENSION pg_textsearch;
