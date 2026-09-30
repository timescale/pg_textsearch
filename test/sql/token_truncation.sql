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

CREATE TABLE trunc_oversized (
    id integer PRIMARY KEY,
    body text NOT NULL
);
INSERT INTO trunc_oversized VALUES
    (1, repeat('a', 256 * 1024) ||
        repeat('b', 256 * 1024) ||
        repeat('a', 256 * 1024)),
    (2, 'http://example.com/' || repeat('x', 300000) ||
        ' continuation'),
    (3, repeat('a', 16));
CREATE INDEX trunc_oversized_idx ON trunc_oversized USING bm25(body)
    WITH (text_config = 'simple', max_token_length = 16);

EXPLAIN (COSTS OFF)
SELECT id
FROM trunc_oversized
ORDER BY body <@> to_bm25query(
    'aaaaaaaaaaaaaaaa', 'trunc_oversized_idx')
LIMIT 1;

SELECT (
    SELECT body <@> to_bm25query(
        'aaaaaaaaaaaaaaaa', 'trunc_oversized_idx')
    FROM trunc_oversized WHERE id = 1
) = (
    SELECT body <@> to_bm25query(
        'aaaaaaaaaaaaaaaa', 'trunc_oversized_idx')
    FROM trunc_oversized WHERE id = 3
) AS oversized_token_has_one_prefix;
SELECT count(*) = 0 AS oversized_token_has_no_suffix_match
FROM (
    SELECT id
    FROM trunc_oversized
    ORDER BY body <@> to_bm25query(
        'bbbbbbbbbbbbbbbb', 'trunc_oversized_idx')
) ranked;
SELECT array_agg(id ORDER BY id) = ARRAY[2]
       AS oversized_url_keeps_following_token
FROM (
    SELECT id
    FROM trunc_oversized
    ORDER BY body <@> to_bm25query(
        'continuation', 'trunc_oversized_idx')
) ranked;

CREATE TABLE trunc_compact (
    id integer PRIMARY KEY,
    body text NOT NULL
);
INSERT INTO trunc_compact
SELECT 1, string_agg(
    'u' || lpad(g::text, 6, '0'), ',' ORDER BY g)
FROM generate_series(1, 160000) g;
CREATE INDEX trunc_compact_idx ON trunc_compact USING bm25(body)
    WITH (text_config = 'simple');
SELECT id = 1 AS compact_unique_tokens_exceed_tsvector_limit
FROM trunc_compact
ORDER BY body <@> to_bm25query('u160000', 'trunc_compact_idx')
LIMIT 1;

CREATE TABLE trunc_frequency_windows (
    body text NOT NULL
);
INSERT INTO trunc_frequency_windows
SELECT repeat('alpha ', 100000);
CREATE INDEX trunc_frequency_windows_idx
ON trunc_frequency_windows USING bm25(body)
WITH (text_config = 'simple');
SELECT split_part(
    split_part(
        bm25_dump_index('trunc_frequency_windows_idx'),
        'total_len: ',
        2),
    E'\n',
    1) = '765' AS repeated_term_preserves_legacy_windows;
SELECT bm25_dump_index('trunc_frequency_windows_idx')
       LIKE '%max_tf=765,%'
       AS repeated_term_preserves_legacy_tf;

CREATE TABLE trunc_frequency_boundary (
    body text NOT NULL
);
INSERT INTO trunc_frequency_boundary
SELECT repeat('alpha ', 43690) ||
       'edge edge ' ||
       repeat('alpha ', 43690);
CREATE INDEX trunc_frequency_boundary_idx
ON trunc_frequency_boundary USING bm25(body)
WITH (text_config = 'simple');
SELECT split_part(
    split_part(
        bm25_dump_index('trunc_frequency_boundary_idx'),
        'total_len: ',
        2),
    E'\n',
    1) = '513' AS boundary_terms_preserve_legacy_length;
SELECT bm25_dump_index('trunc_frequency_boundary_idx')
       LIKE '%max_tf=511,%'
       AS boundary_repeated_term_preserves_legacy_tf;
SELECT bm25_dump_index('trunc_frequency_boundary_idx')
       LIKE '%max_tf=2,%'
       AS boundary_distinct_term_preserves_legacy_tf;

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

CREATE TEXT SEARCH PARSER trunc_native_parser (
    START = pg_catalog.prsd_start,
    GETTOKEN = pg_catalog.prsd_nexttoken,
    END = pg_catalog.prsd_end,
    LEXTYPES = pg_catalog.prsd_lextype,
    HEADLINE = pg_catalog.prsd_headline
);
CREATE TEXT SEARCH CONFIGURATION trunc_custom_parser_cfg (
    PARSER = trunc_native_parser
);
ALTER TEXT SEARCH CONFIGURATION trunc_custom_parser_cfg
    ADD MAPPING FOR asciiword WITH pg_catalog.simple;

CREATE TABLE trunc_custom_parser (
    id integer PRIMARY KEY,
    body text NOT NULL
);
INSERT INTO trunc_custom_parser VALUES
    (1, repeat('a', 300000) || ' continuation'),
    (2, repeat('alpha ', 100000));
CREATE INDEX trunc_custom_parser_idx
ON trunc_custom_parser USING bm25(body)
WITH (text_config = 'public.trunc_custom_parser_cfg');

SELECT count(*) = 0 AS custom_parser_preserves_legacy_elision
FROM (
    SELECT id
    FROM trunc_custom_parser
    ORDER BY body <@> to_bm25query(
        repeat('a', 300), 'trunc_custom_parser_idx')
) ranked;
SELECT id = 1 AS custom_parser_keeps_following_token
FROM trunc_custom_parser
ORDER BY body <@> to_bm25query(
    'continuation', 'trunc_custom_parser_idx')
LIMIT 1;
SELECT split_part(
    split_part(
        bm25_dump_index('trunc_custom_parser_idx'),
        'total_len: ',
        2),
    E'\n',
    1) = '766' AS custom_parser_preserves_legacy_frequency_windows;

INSERT INTO trunc_custom_parser VALUES
    (3, repeat('b', 300000) || ' inserted');
SELECT count(*) = 0 AS custom_parser_dml_preserves_legacy_elision
FROM (
    SELECT id
    FROM trunc_custom_parser
    ORDER BY body <@> to_bm25query(
        repeat('b', 300), 'trunc_custom_parser_idx')
) ranked;
SELECT id = 3 AS custom_parser_dml_keeps_following_token
FROM trunc_custom_parser
ORDER BY body <@> to_bm25query('inserted', 'trunc_custom_parser_idx')
LIMIT 1;

REINDEX INDEX trunc_custom_parser_idx;
SELECT count(*) = 0 AS custom_parser_reindex_preserves_legacy_elision
FROM (
    SELECT id
    FROM trunc_custom_parser
    ORDER BY body <@> to_bm25query(
        repeat('a', 300), 'trunc_custom_parser_idx')
) ranked;

\set VERBOSITY terse
CREATE INDEX trunc_custom_parser_explicit_idx
ON trunc_custom_parser USING bm25(body)
WITH (
    text_config = 'public.trunc_custom_parser_cfg',
    max_token_length = 32
);
ALTER INDEX trunc_custom_parser_idx SET (max_token_length = 32);
\set VERBOSITY default

CREATE TEXT SEARCH CONFIGURATION trunc_custom_dictionary_cfg (
    COPY = pg_catalog.simple
);
CREATE TEXT SEARCH DICTIONARY trunc_custom_dictionary (
    TEMPLATE = pg_catalog.simple
);
ALTER TEXT SEARCH CONFIGURATION trunc_custom_dictionary_cfg
    ALTER MAPPING FOR asciiword WITH trunc_custom_dictionary;
CREATE TABLE trunc_custom_dictionary (
    id integer PRIMARY KEY,
    body text NOT NULL
);
INSERT INTO trunc_custom_dictionary VALUES
    (1, 'abcdefghij'),
    (2, 'abcdezzzzz');
CREATE INDEX trunc_custom_dictionary_idx
ON trunc_custom_dictionary USING bm25(body)
WITH (
    text_config = 'public.trunc_custom_dictionary_cfg',
    max_token_length = 5
);
SELECT array_agg(id ORDER BY id) = ARRAY[1, 2]
       AS builtin_parser_custom_dictionary_truncates
FROM (
    SELECT id
    FROM trunc_custom_dictionary
    ORDER BY body <@> to_bm25query(
        'abcdefghij', 'trunc_custom_dictionary_idx')
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

CREATE TABLE trunc_boolean_clean (
    id integer PRIMARY KEY,
    body text NOT NULL
);
INSERT INTO trunc_boolean_clean
SELECT g, CASE WHEN g = 7777 THEN 'needle' ELSE 'common' END
FROM generate_series(1, 10000) g;
CREATE INDEX trunc_boolean_clean_idx ON trunc_boolean_clean USING bm25(body)
    WITH (text_config = 'simple');
ANALYZE trunc_boolean_clean;

RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_seqscan;
EXPLAIN (COSTS OFF)
SELECT id
FROM trunc_boolean_clean
WHERE body @@ to_tsquery('simple', 'needle');
SELECT array_agg(id ORDER BY id) = ARRAY[7777]
       AS unchanged_normalization_stays_selective
FROM trunc_boolean_clean
WHERE body @@ to_tsquery('simple', 'needle');

SET plan_cache_mode = force_generic_plan;
PREPARE trunc_boolean_cached AS
SELECT id
FROM trunc_boolean_clean
WHERE body @@ to_tsquery('simple', 'needle');
EXPLAIN (COSTS OFF)
EXECUTE trunc_boolean_cached;

INSERT INTO trunc_boolean_clean VALUES
    (10001, repeat('z', 300));
EXPLAIN (COSTS OFF)
EXECUTE trunc_boolean_cached;
EXECUTE trunc_boolean_cached;
DEALLOCATE trunc_boolean_cached;
RESET plan_cache_mode;

ANALYZE trunc_boolean_clean;
EXPLAIN (COSTS OFF)
SELECT id
FROM trunc_boolean_clean
WHERE body @@ to_tsquery('simple', 'needle');

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
DROP TABLE trunc_oversized;
DROP TABLE trunc_compact;
DROP TABLE trunc_frequency_windows;
DROP TABLE trunc_frequency_boundary;
DROP TABLE trunc_boolean;
DROP TABLE trunc_boolean_clean;
DROP TABLE trunc_boolean_dictionary;
DROP TABLE trunc_custom_parser;
DROP TABLE trunc_custom_dictionary;
DROP TEXT SEARCH CONFIGURATION trunc_custom_parser_cfg;
DROP TEXT SEARCH PARSER trunc_native_parser;
DROP TEXT SEARCH CONFIGURATION trunc_custom_dictionary_cfg;
DROP TEXT SEARCH DICTIONARY trunc_custom_dictionary;
DROP EXTENSION pg_textsearch CASCADE;
