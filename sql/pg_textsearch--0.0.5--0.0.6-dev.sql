-- Upgrade from 0.0.5 to 0.0.6-dev

-- Create the new scoring functions with bm25_ prefix
CREATE FUNCTION bm25_text_bm25query_score(left_text text, right_query bm25query)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_text_bm25query_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

CREATE FUNCTION bm25_text_text_score(text, text) RETURNS float8
    AS 'MODULE_PATHNAME', 'bm25_text_text_score'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- Drop the operator class first (it depends on the operator)
-- Note: Users must rebuild their indexes after this upgrade
DROP OPERATOR CLASS IF EXISTS text_bm25_ops USING bm25 CASCADE;

-- Drop the old operators
DROP OPERATOR IF EXISTS <@> (text, bm25query);
DROP OPERATOR IF EXISTS <@> (text, text);

-- Drop the old functions
DROP FUNCTION IF EXISTS text_bm25query_score(text, bm25query);
DROP FUNCTION IF EXISTS text_text_score(text, text);

-- Create new operators with the bm25_ prefixed functions
CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = bm25query,
    PROCEDURE = bm25_text_bm25query_score
);

CREATE OPERATOR <@> (
    LEFTARG = text,
    RIGHTARG = text,
    PROCEDURE = bm25_text_text_score
);

-- Recreate the operator class with the new functions
CREATE OPERATOR CLASS text_bm25_ops
DEFAULT FOR TYPE text USING bm25 AS
    OPERATOR    1   <@> (text, bm25query) FOR ORDER BY float_ops,
    FUNCTION    8   (text, bm25query)   bm25_text_bm25query_score(text, bm25query);
