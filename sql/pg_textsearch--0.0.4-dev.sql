-- pg_textsearch extension version 0.0.4-dev

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_textsearch" to load this file. \quit

-- Access method

CREATE FUNCTION tp_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'tp_handler'
LANGUAGE C;

CREATE ACCESS METHOD bm25 TYPE INDEX HANDLER tp_handler;

-- bm25vector type

CREATE FUNCTION bm25vector_in(cstring)
RETURNS bm25vector
AS 'MODULE_PATHNAME', 'tpvector_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION bm25vector_out(bm25vector)
RETURNS cstring
AS 'MODULE_PATHNAME', 'tpvector_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION bm25vector_recv(internal)
RETURNS bm25vector
AS 'MODULE_PATHNAME', 'tpvector_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION bm25vector_send(bm25vector)
RETURNS bytea
AS 'MODULE_PATHNAME', 'tpvector_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE bm25vector (
    INPUT = bm25vector_in,
    OUTPUT = bm25vector_out,
    RECEIVE = bm25vector_recv,
    SEND = bm25vector_send,
    STORAGE = extended,
    ALIGNMENT = int4
);

-- Convert text to a bm25vector, using specified index
CREATE FUNCTION to_bm25vector(input_text text, index_name text)
RETURNS bm25vector
AS 'MODULE_PATHNAME', 'to_tpvector'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- bm25query type

CREATE FUNCTION bm25query_in(cstring)
RETURNS bm25query
AS 'MODULE_PATHNAME', 'tpquery_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION bm25query_out(bm25query)
RETURNS cstring
AS 'MODULE_PATHNAME', 'tpquery_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION bm25query_recv(internal)
RETURNS bm25query
AS 'MODULE_PATHNAME', 'tpquery_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION bm25query_send(bm25query)
RETURNS bytea
AS 'MODULE_PATHNAME', 'tpquery_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE bm25query (
    INPUT = bm25query_in,
    OUTPUT = bm25query_out,
    RECEIVE = bm25query_recv,
    SEND = bm25query_send,
    STORAGE = extended,
    ALIGNMENT = int4
);

-- Convert text to bm25query
CREATE FUNCTION to_bm25query(input_text text)
RETURNS bm25query
AS 'MODULE_PATHNAME', 'to_tpquery_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION to_bm25query(input_text text, index_name text)
RETURNS bm25query
AS 'MODULE_PATHNAME', 'to_tpquery_text_index'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;


-- Equality function: bm25vector = bm25vector → boolean
CREATE FUNCTION bm25vector_eq(bm25vector, bm25vector)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tpvector_eq'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the = operator for equality
CREATE OPERATOR = (
    LEFTARG = bm25vector,
    RIGHTARG = bm25vector,
    FUNCTION = bm25vector_eq,
    COMMUTATOR = =,
    HASHES
);


-- BM25 scoring function for text <@> bm25query operations
CREATE FUNCTION text_bm25query_score(left_text text, right_query bm25query)
RETURNS float8
AS 'MODULE_PATHNAME', 'text_tpquery_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- bm25query equality function
CREATE FUNCTION bm25query_eq(bm25query, bm25query)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tpquery_eq'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;



-- <@> operator for text <@> bm25query operations
CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = bm25query,
    PROCEDURE = text_bm25query_score
);

-- = operator for bm25query equality
CREATE OPERATOR = (
    LEFTARG = bm25query,
    RIGHTARG = bm25query,
    FUNCTION = bm25query_eq,
    COMMUTATOR = =,
    HASHES
);

-- Support function for calculating BM25 distances (negative scores for ASC ordering)
CREATE FUNCTION bm25_distance(text, bm25query) RETURNS float8
    AS 'MODULE_PATHNAME', 'tp_distance'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- bm25 operator class for text columns
CREATE OPERATOR CLASS text_bm25_ops
DEFAULT FOR TYPE text USING bm25 AS
    OPERATOR    1   <@> (text, bm25query) FOR ORDER BY float_ops,
    FUNCTION    8   bm25_distance(text, bm25query);

-- Debug function to dump index contents
CREATE FUNCTION bm25_debug_dump_index(text, boolean DEFAULT false) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_debug_dump_index'
    LANGUAGE C STRICT STABLE;

-- Clear the BM25 index registry (used by validation tests)
CREATE FUNCTION bm25_clear_registry() RETURNS void
    AS 'MODULE_PATHNAME', 'bm25_clear_registry'
    LANGUAGE C STRICT VOLATILE;

-- Display warning about prerelease status
DO $$
BEGIN
    RAISE INFO 'pg_textsearch v0.0.4-dev: This is prerelease software and should not be used in production.';
END
$$;
