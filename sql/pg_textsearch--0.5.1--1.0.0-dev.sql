-- Upgrade from 0.5.1 to 1.0.0-dev
--
-- Starting with 1.0.0-dev, the shared library includes the version in
-- its filename (e.g., pg_textsearch-1.0.0-dev.so). All C functions must
-- be recreated so their probin references the versioned library path.

-- Recreate all C functions to update probin to versioned library
CREATE OR REPLACE FUNCTION tp_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'tp_handler'
LANGUAGE C;

CREATE OR REPLACE FUNCTION bm25vector_in(cstring)
RETURNS bm25vector
AS 'MODULE_PATHNAME', 'tpvector_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25vector_out(bm25vector)
RETURNS cstring
AS 'MODULE_PATHNAME', 'tpvector_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25vector_recv(internal)
RETURNS bm25vector
AS 'MODULE_PATHNAME', 'tpvector_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25vector_send(bm25vector)
RETURNS bytea
AS 'MODULE_PATHNAME', 'tpvector_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25query_in(cstring)
RETURNS bm25query
AS 'MODULE_PATHNAME', 'tpquery_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25query_out(bm25query)
RETURNS cstring
AS 'MODULE_PATHNAME', 'tpquery_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25query_recv(internal)
RETURNS bm25query
AS 'MODULE_PATHNAME', 'tpquery_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25query_send(bm25query)
RETURNS bytea
AS 'MODULE_PATHNAME', 'tpquery_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION to_bm25query(input_text text)
RETURNS bm25query
AS 'MODULE_PATHNAME', 'to_tpquery_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION to_bm25query(input_text text, index_name text)
RETURNS bm25query
AS 'MODULE_PATHNAME', 'to_tpquery_text_index'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25vector_eq(bm25vector, bm25vector)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tpvector_eq'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25_text_bm25query_score(
    left_text text, right_query bm25query)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_text_bm25query_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

CREATE OR REPLACE FUNCTION bm25query_eq(bm25query, bm25query)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tpquery_eq'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25_text_text_score(text, text)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_text_text_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

CREATE OR REPLACE FUNCTION bm25_get_current_score()
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_get_current_score'
LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bm25_dump_index(text)
RETURNS text
AS 'MODULE_PATHNAME', 'tp_dump_index'
LANGUAGE C STRICT STABLE;

CREATE OR REPLACE FUNCTION bm25_dump_index(text, text)
RETURNS text
AS 'MODULE_PATHNAME', 'tp_dump_index'
LANGUAGE C STRICT STABLE;

CREATE OR REPLACE FUNCTION bm25_spill_index(index_name text)
RETURNS int4
AS 'MODULE_PATHNAME', 'tp_spill_memtable'
LANGUAGE C VOLATILE STRICT;

CREATE OR REPLACE FUNCTION bm25_summarize_index(text)
RETURNS text
AS 'MODULE_PATHNAME', 'tp_summarize_index'
LANGUAGE C STRICT STABLE;

CREATE OR REPLACE FUNCTION bm25_debug_pageviz(
    index_name text, filepath text)
RETURNS text
AS 'MODULE_PATHNAME', 'tp_debug_pageviz'
LANGUAGE C STRICT STABLE;

DO $$
BEGIN
    RAISE WARNING 'pg_textsearch v1.0.0-dev is a prerelease. Do not use in production.';
END
$$;
