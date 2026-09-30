\echo Use "CREATE EXTENSION pg_textsearch_test" to load this file. \quit

CREATE FUNCTION pg_textsearch_test_move_level(
    idx regclass, source_level integer, destination_level integer)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_move_level'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_set_tombstone_link(
    idx regclass, at_head boolean, block bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_set_tombstone_link'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_panic(point text DEFAULT NULL)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_panic'
LANGUAGE C PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_worker_panic(
    point text DEFAULT NULL)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_worker_panic'
LANGUAGE C PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_segment_limit(limit_value integer)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_segment_limit'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_reclaim_horizon_hold()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_reclaim_horizon_hold'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_query_hint_roundtrip(query text, k bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_textsearch_test_query_hint_roundtrip'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_legacy_segment(total_tokens bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_legacy_segment'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_v5_segment_total_len(
    total_tokens bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_v5_segment_total_len'
LANGUAGE C STRICT PARALLEL UNSAFE;

CREATE FUNCTION pg_textsearch_test_attach_vacuum_total_len(
    total_tokens bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_textsearch_test_attach_vacuum_total_len'
LANGUAGE C STRICT PARALLEL UNSAFE;
