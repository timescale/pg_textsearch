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

SELECT count(*) AS broad_prefix_matches
FROM boolean_broad_prefix_docs
WHERE body @@ to_tsquery('simple', 'broadprefix:*');

RESET enable_seqscan;
DROP TABLE boolean_broad_prefix_docs;

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

SET enable_seqscan = off;

\pset format unaligned
SELECT array_agg(id ORDER BY id) AS galloping_seek_matches
FROM boolean_seek_docs
WHERE body @@ to_tsquery('english', 'anchor & common');
\pset format aligned

RESET enable_seqscan;
DROP TABLE boolean_seek_docs;
DROP EXTENSION pg_textsearch;
