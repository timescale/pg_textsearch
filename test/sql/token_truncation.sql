CREATE EXTENSION pg_textsearch;

SET client_min_messages = WARNING;
SET enable_seqscan = off;

CREATE TABLE trunc_urls (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO trunc_urls VALUES
    (1, 'http://example.com/abcdefghijklmnop-one'),
    (2, 'http://example.com/abcdefghijklmnop-two'),
    (3, 'http://example.com/unrelated');

CREATE INDEX trunc_urls_idx ON trunc_urls USING bm25(body)
    WITH (text_config = 'simple', max_token_length = 32);

SELECT array_agg(id ORDER BY id) = ARRAY[1, 2] AS url_prefix_matches
FROM (
    SELECT id
    FROM trunc_urls
    ORDER BY body <@> to_bm25query(
        'http://example.com/abcdefghijklmnop-one',
        'trunc_urls_idx')
    LIMIT 2
) ranked;

SELECT (
    SELECT round((body <@> to_bm25query(
        'http://example.com/abcdefghijklmnop-one',
        'trunc_urls_idx'))::numeric, 6)
    FROM trunc_urls
    WHERE id = 1
) = (
    SELECT round((body <@> to_bm25query(
        'http://example.com/abcdefghijklmnop-one',
        'trunc_urls_idx'))::numeric, 6)
    FROM trunc_urls
    ORDER BY body <@> to_bm25query(
        'http://example.com/abcdefghijklmnop-one',
        'trunc_urls_idx')
    LIMIT 1
) AS standalone_ranked_parity;

INSERT INTO trunc_urls VALUES
    (4, 'http://example.com/abcdefghijklmnop-three');
UPDATE trunc_urls
SET body = 'http://example.com/abcdefghijklmnop-four'
WHERE id = 4;

SELECT id = 4 AS dml_uses_index_limit
FROM trunc_urls
ORDER BY body <@> to_bm25query(
    'http://example.com/abcdefghijklmnop-four',
    'trunc_urls_idx')
LIMIT 1;

CREATE TABLE trunc_boundaries (
    id integer PRIMARY KEY,
    body text NOT NULL
);
INSERT INTO trunc_boundaries VALUES
    (1, repeat('a', 254)),
    (2, repeat('a', 255)),
    (3, repeat('a', 256)),
    (4, 'http://example.com/' || repeat('x', 2200)),
    (5, ''),
    (6, 'éé'),
    (7, 'ééé'),
    (8, 'é alpha'),
    (9, 'abcdefghij'),
    (10, 'abcdzzzzzz'),
    (11, 'the running');
CREATE INDEX trunc_default_idx ON trunc_boundaries USING bm25(body)
    WITH (text_config = 'simple');

SELECT array_agg(id ORDER BY id) = ARRAY[1]
       AS default_keeps_254_and_255_distinct
FROM (
    SELECT id
    FROM trunc_boundaries
    ORDER BY body <@> to_bm25query(repeat('a', 254), 'trunc_default_idx')
) ranked;
SELECT array_agg(id ORDER BY id) = ARRAY[2, 3]
       AS default_truncates_256_to_255
FROM (
    SELECT id
    FROM trunc_boundaries
    ORDER BY body <@> to_bm25query(repeat('a', 256), 'trunc_default_idx')
) ranked;
SELECT array_agg(id ORDER BY id) = ARRAY[4]
       AS over_2047_url_retained
FROM (
    SELECT id
    FROM trunc_boundaries
    ORDER BY body <@> to_bm25query(
        'http://example.com/' || repeat('x', 2200),
        'trunc_default_idx')
) ranked;
SELECT count(*) = 0 AS tokenless_input_stays_empty
FROM (
    SELECT id
    FROM trunc_boundaries
    ORDER BY body <@> to_bm25query('missing', 'trunc_default_idx')
) ranked;
DROP INDEX trunc_default_idx;

CREATE INDEX trunc_utf8_idx ON trunc_boundaries USING bm25(body)
    WITH (text_config = 'simple', max_token_length = 5);
SELECT array_agg(id ORDER BY id) = ARRAY[6, 7]
       AS utf8_clips_at_character_boundary
FROM (
    SELECT id
    FROM trunc_boundaries
    ORDER BY body <@> to_bm25query('ééé', 'trunc_utf8_idx')
) ranked;
DROP INDEX trunc_utf8_idx;

CREATE INDEX trunc_one_byte_idx ON trunc_boundaries USING bm25(body)
    WITH (text_config = 'simple', max_token_length = 1);
SELECT 8 = ANY(array_agg(id ORDER BY id))
       AS too_wide_character_does_not_drop_later_tokens
FROM (
    SELECT id
    FROM trunc_boundaries
    ORDER BY body <@> to_bm25query('alpha', 'trunc_one_byte_idx')
) ranked;
DROP INDEX trunc_one_byte_idx;

CREATE INDEX trunc_four_idx ON trunc_boundaries USING bm25(body)
    WITH (text_config = 'simple', max_token_length = 4);
CREATE TABLE trunc_boundaries_eight AS TABLE trunc_boundaries;
CREATE INDEX trunc_eight_idx ON trunc_boundaries_eight USING bm25(body)
    WITH (text_config = 'simple', max_token_length = 8);

SELECT (
    SELECT array_agg(id ORDER BY id)
    FROM (
        SELECT id
        FROM trunc_boundaries
        ORDER BY body <@> to_bm25query('abcdefghij', 'trunc_four_idx')
    ) ranked
) = ARRAY[9, 10]
AND (
    SELECT array_agg(id ORDER BY id)
    FROM (
        SELECT id
        FROM trunc_boundaries_eight
        ORDER BY body <@> to_bm25query('abcdefghij', 'trunc_eight_idx')
    ) ranked
) = ARRAY[9]
AS per_index_limits_coexist;
DROP INDEX trunc_four_idx;
DROP TABLE trunc_boundaries_eight;

CREATE INDEX trunc_english_idx ON trunc_boundaries USING bm25(body)
    WITH (text_config = 'english', max_token_length = 32);
SELECT array_agg(id ORDER BY id) = ARRAY[11]
       AS dictionary_chain_is_preserved
FROM (
    SELECT id
    FROM trunc_boundaries
    ORDER BY body <@> to_bm25query('the running', 'trunc_english_idx')
) ranked;

\set VERBOSITY terse
CREATE INDEX trunc_invalid_zero_idx ON trunc_boundaries USING bm25(body)
    WITH (text_config = 'simple', max_token_length = 0);
CREATE INDEX trunc_invalid_large_idx ON trunc_boundaries USING bm25(body)
    WITH (text_config = 'simple', max_token_length = 2048);
\set VERBOSITY default

CREATE TABLE trunc_boolean (
    id integer PRIMARY KEY,
    body text NOT NULL
);

INSERT INTO trunc_boolean VALUES
    (1, 'alpha aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'),
    (2, 'alpha aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab'),
    (3, 'beta');

SET default_text_search_config = 'pg_catalog.simple';
CREATE INDEX trunc_boolean_idx ON trunc_boolean USING bm25(body)
    WITH (text_config = 'simple', max_token_length = 16);

SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_seqscan = on;
CREATE TEMP TABLE trunc_boolean_seq AS
SELECT 'exact' AS kind, array_agg(id ORDER BY id) AS ids
FROM trunc_boolean
WHERE body @@ to_tsquery(
    'simple', 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
UNION ALL
SELECT 'prefix', array_agg(id ORDER BY id)
FROM trunc_boolean
WHERE body @@ to_tsquery('simple', 'aaaaaaaaaa:*')
UNION ALL
SELECT 'phrase', array_agg(id ORDER BY id)
FROM trunc_boolean
WHERE body @@ to_tsquery(
    'simple', 'alpha <-> aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
UNION ALL
SELECT 'negation', array_agg(id ORDER BY id)
FROM trunc_boolean
WHERE body @@ to_tsquery(
    'simple', '!aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');

SET enable_indexscan = on;
SET enable_bitmapscan = on;
SET enable_seqscan = off;

EXPLAIN (COSTS OFF)
SELECT id
FROM trunc_boolean
WHERE body @@ to_tsquery(
    'simple', 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');

SELECT bool_and(seq.ids IS NOT DISTINCT FROM idx.ids)
       AS native_boolean_seq_index_parity
FROM trunc_boolean_seq seq
JOIN (
    SELECT 'exact' AS kind, array_agg(id ORDER BY id) AS ids
    FROM trunc_boolean
    WHERE body @@ to_tsquery(
        'simple', 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
    UNION ALL
    SELECT 'prefix', array_agg(id ORDER BY id)
    FROM trunc_boolean
    WHERE body @@ to_tsquery('simple', 'aaaaaaaaaa:*')
    UNION ALL
    SELECT 'phrase', array_agg(id ORDER BY id)
    FROM trunc_boolean
    WHERE body @@ to_tsquery(
        'simple', 'alpha <-> aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
    UNION ALL
    SELECT 'negation', array_agg(id ORDER BY id)
    FROM trunc_boolean
    WHERE body @@ to_tsquery(
        'simple', '!aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
) idx USING (kind);

CREATE TABLE trunc_boolean_dictionary (
    id integer PRIMARY KEY,
    body text NOT NULL
);
INSERT INTO trunc_boolean_dictionary VALUES
    (1, 'running'),
    (2, 'walking');
SET default_text_search_config = 'pg_catalog.english';
CREATE INDEX trunc_boolean_dictionary_idx
ON trunc_boolean_dictionary USING bm25(body)
WITH (text_config = 'english', max_token_length = 5);

SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_seqscan = on;
CREATE TEMP TABLE trunc_boolean_dictionary_seq AS
SELECT array_agg(id ORDER BY id) AS ids
FROM trunc_boolean_dictionary
WHERE body @@ to_tsquery('english', 'running');

SET enable_indexscan = on;
SET enable_bitmapscan = on;
SET enable_seqscan = off;
SELECT seq.ids IS NOT DISTINCT FROM idx.ids
       AS dictionary_boolean_seq_index_parity
FROM trunc_boolean_dictionary_seq seq
CROSS JOIN (
    SELECT array_agg(id ORDER BY id) AS ids
    FROM trunc_boolean_dictionary
    WHERE body @@ to_tsquery('english', 'running')
) idx;

SET default_text_search_config = 'pg_catalog.simple';
ALTER INDEX trunc_boolean_idx SET (max_token_length = 32);
\set VERBOSITY terse
SELECT id
FROM trunc_boolean
ORDER BY body <@> to_bm25query('alpha', 'trunc_boolean_idx')
LIMIT 1;
\set VERBOSITY default

REINDEX INDEX trunc_boolean_idx;
SELECT id = 1 AS reindex_adopts_changed_option
FROM trunc_boolean
ORDER BY body <@> to_bm25query(
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'trunc_boolean_idx')
LIMIT 1;

ALTER INDEX trunc_boolean_idx RESET (max_token_length);
\set VERBOSITY terse
SELECT id
FROM trunc_boolean
ORDER BY body <@> to_bm25query('alpha', 'trunc_boolean_idx')
LIMIT 1;
\set VERBOSITY default
REINDEX INDEX trunc_boolean_idx;
SELECT id = 1 AS reindex_adopts_reset_default
FROM trunc_boolean
ORDER BY body <@> to_bm25query(
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
    'trunc_boolean_idx')
LIMIT 1;

DROP TABLE trunc_urls;
DROP TABLE trunc_boundaries;
DROP TABLE trunc_boolean;
DROP TABLE trunc_boolean_dictionary;
DROP EXTENSION pg_textsearch CASCADE;
