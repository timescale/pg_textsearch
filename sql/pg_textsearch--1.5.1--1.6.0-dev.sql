-- Upgrade from 1.5.1 to 1.6.0-dev

-- Verify the library is loaded. The version-equality check lives in
-- the main install file (pg_textsearch--<default_version>.sql); upgrade
-- scripts must accept any loaded version because they may run as
-- intermediate steps in a chain that ends at default_version, not at
-- the version named in this filename.
DO $$
BEGIN
    IF pg_catalog.current_setting('pg_textsearch.library_version', true)
       IS NULL THEN
        RAISE EXCEPTION
            'pg_textsearch library not loaded. '
            'Add pg_textsearch to shared_preload_libraries and restart.';
    END IF;
END $$;

-- Planner-only overloads carrying the scored row's physical partition.  SQL
-- cannot construct an internal value, so callers cannot name a partition.
CREATE FUNCTION @extschema@.bm25_text_bm25query_score(
    text, @extschema@.bm25query, internal)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_text_bm25query_score'
LANGUAGE C STABLE STRICT PARALLEL UNSAFE COST 1000;

CREATE FUNCTION @extschema@.bm25_textarray_bm25query_score(
    text[], @extschema@.bm25query, internal)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_textarray_bm25query_score'
LANGUAGE C STABLE STRICT PARALLEL UNSAFE COST 1000;
