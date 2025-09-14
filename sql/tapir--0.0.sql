-- Tapir extension version 0.0

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

-- tpquery type

CREATE FUNCTION tpquery_in(cstring)
RETURNS tpquery
AS 'MODULE_PATHNAME', 'tpquery_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tpquery_out(tpquery)
RETURNS cstring
AS 'MODULE_PATHNAME', 'tpquery_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tpquery_recv(internal)
RETURNS tpquery
AS 'MODULE_PATHNAME', 'tpquery_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION tpquery_send(tpquery)
RETURNS bytea
AS 'MODULE_PATHNAME', 'tpquery_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE tpquery (
    INPUT = tpquery_in,
    OUTPUT = tpquery_out,
    RECEIVE = tpquery_recv,
    SEND = tpquery_send,
    STORAGE = extended,
    ALIGNMENT = int4
);

-- Convert text to tpquery
CREATE FUNCTION to_tpquery(input_text text)
RETURNS tpquery
AS 'MODULE_PATHNAME', 'to_tpquery_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION to_tpquery(input_text text, index_name text)
RETURNS tpquery
AS 'MODULE_PATHNAME', 'to_tpquery_text_index'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Scoring function: tpvector <@> tpvector → double precision
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


-- BM25 scoring function for text <@> tpquery operations
CREATE FUNCTION text_tpquery_score(left_text text, right_query tpquery)
RETURNS float8
AS 'MODULE_PATHNAME', 'text_tpquery_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- tpquery equality function
CREATE FUNCTION tpquery_eq(tpquery, tpquery)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tpquery_eq'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;



-- <@> operator for text <@> tpquery operations
CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = tpquery,
    PROCEDURE = text_tpquery_score
);

-- = operator for tpquery equality
CREATE OPERATOR = (
    LEFTARG = tpquery,
    RIGHTARG = tpquery,
    FUNCTION = tpquery_eq,
    COMMUTATOR = =,
    HASHES
);

-- Tapir operator class for text columns
CREATE OPERATOR CLASS text_tp_ops
DEFAULT FOR TYPE text USING tapir AS
    OPERATOR    1   <@> (text, tpquery) FOR ORDER BY float_ops;

-- Debug function to dump index contents
CREATE FUNCTION tp_debug_dump_index(text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_debug_dump_index'
    LANGUAGE C STRICT STABLE;
