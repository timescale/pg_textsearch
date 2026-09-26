\echo Use "CREATE EXTENSION pg_textsearch_test" to load this file. \quit

CREATE FUNCTION pg_textsearch_test_attach_panic(point text DEFAULT NULL)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_panic'
LANGUAGE C PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_segment_limit(limit_value integer)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_segment_limit'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_reclaim_horizon_hold()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_reclaim_horizon_hold'
LANGUAGE C STRICT PARALLEL UNSAFE;
