-- Upgrade from 0.0.5 to 0.0.6-dev

-- Rename text_bm25query_score to bm25_text_bm25query_score
-- Create the new function with bm25_ prefix
CREATE FUNCTION bm25_text_bm25query_score(left_text text, right_query bm25query)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_text_bm25query_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- Update the text <@> bm25query operator to use the new function
DROP OPERATOR IF EXISTS <@> (text, bm25query);
CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = bm25query,
    PROCEDURE = bm25_text_bm25query_score
);

-- Drop the old function
DROP FUNCTION IF EXISTS text_bm25query_score(text, bm25query);

-- Rename text_text_score to bm25_text_text_score
-- Create the new function with bm25_ prefix
CREATE FUNCTION bm25_text_text_score(text, text) RETURNS float8
    AS 'MODULE_PATHNAME', 'bm25_text_text_score'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- Update the text <@> text operator to use the new function
DROP OPERATOR IF EXISTS <@> (text, text);
CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = text,
    PROCEDURE = bm25_text_text_score
);

-- Drop the old function
DROP FUNCTION IF EXISTS text_text_score(text, text);
