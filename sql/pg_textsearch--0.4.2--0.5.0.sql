-- Upgrade from 0.4.2 to 0.5.0
-- Adds bm25_debug_pageviz() for index page layout visualization

CREATE FUNCTION bm25_debug_pageviz(
    index_name text, filepath text)
RETURNS text
AS 'MODULE_PATHNAME', 'tp_debug_pageviz'
LANGUAGE C STRICT STABLE;

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.5.0';
END
$$;
