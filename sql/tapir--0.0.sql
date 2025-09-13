-- tapir extension version 0.0

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION tapir" to load this file. \quit


-- Access method

CREATE FUNCTION tp_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'tp_handler'
LANGUAGE C;

CREATE ACCESS METHOD tapir TYPE INDEX HANDLER tp_handler;

-- tpvector type

CREATE FUNCTION tpvector_in(cstring)
RETURNS tpvector
AS 'MODULE_PATHNAME', 'tpvector_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tpvector_out(tpvector)
RETURNS cstring
AS 'MODULE_PATHNAME', 'tpvector_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tpvector_recv(internal)
RETURNS tpvector
AS 'MODULE_PATHNAME', 'tpvector_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tpvector_send(tpvector)
RETURNS bytea
AS 'MODULE_PATHNAME', 'tpvector_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE tpvector (
    INPUT = tpvector_in,
    OUTPUT = tpvector_out,
    RECEIVE = tpvector_recv,
    SEND = tpvector_send,
    STORAGE = extended,
    ALIGNMENT = int4
);

-- Convert text to a tpvector, using specified index
CREATE FUNCTION to_tpvector(input_text text, index_name text)
RETURNS tpvector
AS 'MODULE_PATHNAME', 'to_tpvector'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Scoring operator: tpvector <@> tpvector → double precision
CREATE FUNCTION tpvector_score(tpvector, tpvector)
RETURNS double precision
AS 'MODULE_PATHNAME', 'tpvector_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Equality function: tpvector = tpvector → boolean
CREATE FUNCTION tpvector_eq(tpvector, tpvector)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tpvector_eq'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the = operator for equality
CREATE OPERATOR = (
    LEFTARG = tpvector,
    RIGHTARG = tpvector,
    FUNCTION = tpvector_eq,
    COMMUTATOR = =,
    HASHES
);

-- BM25 scoring function for tpvector <@> text operations (document <@> query)
CREATE FUNCTION tpvector_text_score(left_vector tpvector, right_text text)
RETURNS float8
AS 'MODULE_PATHNAME', 'tpvector_text_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- BM25 scoring function for text <@> text operations (primarily for index scans)
CREATE FUNCTION tp_text_text_score(left_text text, right_text text)
RETURNS float8
AS 'MODULE_PATHNAME', 'tp_text_text_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- BM25 scoring function for text <@> tpvector operations (document <@> query, commutative)
CREATE FUNCTION text_tpvector_score(left_text text, right_vector tpvector)
RETURNS float8
AS 'MODULE_PATHNAME', 'text_tpvector_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;


-- <@> operator for tpvector <@> tpvector operations
CREATE OPERATOR <@> (
    LEFTARG = tpvector,
    RIGHTARG = tpvector,
    PROCEDURE = tpvector_score
);

-- <@> operator for tpvector <@> text operations (standalone document <@> query)
CREATE OPERATOR <@> (
    LEFTARG = tpvector,
    RIGHTARG = text,
    PROCEDURE = tpvector_text_score
);

-- <@> operator for text <@> text operations (index scan document <@> query)
CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = text,
    PROCEDURE = tp_text_text_score
);

-- <@> operator for text <@> tpvector operations (standalone document <@> query)
CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = tpvector,
    PROCEDURE = text_tpvector_score
);

-- Tapir operator class for text columns
CREATE OPERATOR CLASS text_tp_ops
DEFAULT FOR TYPE text USING tapir AS
    OPERATOR    1   <@> (text, text) FOR ORDER BY float_ops,
    OPERATOR    1   <@> (text, tpvector) FOR ORDER BY float_ops;

-- Debug function to dump index contents
CREATE FUNCTION tp_debug_dump_index(text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_debug_dump_index'
    LANGUAGE C STRICT STABLE;

-- DSA-based system doesn't need memory pool monitoring
-- Memory usage is managed dynamically by PostgreSQL's DSA subsystem
