-- pg_textsearch extension version 0.2.0

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
--
-- COST 1000: Standalone scoring is expensive. Each call parses document text
-- with to_tsvector (~14μs per doc), opens the index, looks up IDF values, and
-- calculates BM25 scores. High cost helps planner prefer index scans.
CREATE FUNCTION bm25_text_bm25query_score(left_text text, right_query bm25query)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_text_bm25query_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- bm25query equality function
CREATE FUNCTION bm25query_eq(bm25query, bm25query)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tpquery_eq'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;



-- <@> operator for text <@> bm25query operations
CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = bm25query,
    PROCEDURE = bm25_text_bm25query_score
);

-- Function for text <@> text operator (planner hook rewrites to text <@> bm25query)
-- COST 1000: High cost makes planner prefer index scans over seq scan + sort.
-- In practice, this function errors without index scan context, but the cost
-- helps the planner choose the right path before execution.
CREATE FUNCTION bm25_text_text_score(text, text) RETURNS float8
    AS 'MODULE_PATHNAME', 'bm25_text_text_score'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- Stub function returning cached score from index scan.
-- The planner hook replaces resjunk ORDER BY expressions with calls to this
-- function, avoiding expensive re-computation of BM25 scores.
CREATE FUNCTION bm25_get_current_score() RETURNS float8
    AS 'MODULE_PATHNAME', 'bm25_get_current_score'
    LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

-- <@> operator for text <@> text operations (implicit index resolution)
-- The planner hook transforms this to text <@> bm25query when a BM25 index exists
CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = text,
    PROCEDURE = bm25_text_text_score
);

-- = operator for bm25query equality
CREATE OPERATOR = (
    LEFTARG = bm25query,
    RIGHTARG = bm25query,
    FUNCTION = bm25query_eq,
    COMMUTATOR = =,
    HASHES
);

-- bm25 operator class for text columns
-- The planner hook rewrites text <@> text to text <@> bm25query, so we only
-- need to register the bm25query operator and support function here.
CREATE OPERATOR CLASS text_bm25_ops
DEFAULT FOR TYPE text USING bm25 AS
    OPERATOR    1   <@> (text, bm25query) FOR ORDER BY float_ops,
    FUNCTION    8   (text, bm25query)   bm25_text_bm25query_score(text, bm25query);

-- Debug function to dump index contents (memtable and segments)
-- Single argument version returns truncated output as text
CREATE FUNCTION bm25_dump_index(text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_dump_index'
    LANGUAGE C STRICT STABLE;

-- Two argument version writes full dump (with hex) to file
CREATE FUNCTION bm25_dump_index(text, text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_dump_index'
    LANGUAGE C STRICT STABLE;

-- Display warning about prerelease status
DO $$
BEGIN
    RAISE INFO 'pg_textsearch v0.2.0: This is prerelease software and should not be used in production.';
    RAISE INFO 'This release contains breaking changes in the bm25 index structure and will require existing indexes to be rebuilt.';
END
$$;

-- Function to force segment write (spill memtable to disk)
CREATE OR REPLACE FUNCTION bm25_spill_index(index_name text)
RETURNS int4
AS 'MODULE_PATHNAME', 'tp_spill_memtable'
LANGUAGE C VOLATILE STRICT;

-- Fast summary function showing only statistics (no content dump)
CREATE FUNCTION bm25_summarize_index(text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_summarize_index'
    LANGUAGE C STRICT STABLE;
