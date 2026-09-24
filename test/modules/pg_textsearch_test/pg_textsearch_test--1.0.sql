\echo Use "CREATE EXTENSION pg_textsearch_test" to load this file. \quit

CREATE FUNCTION pg_textsearch_test_attach_panic()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_panic'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_segment_limit(limit_value integer)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_segment_limit'
LANGUAGE C STRICT PARALLEL UNSAFE;
