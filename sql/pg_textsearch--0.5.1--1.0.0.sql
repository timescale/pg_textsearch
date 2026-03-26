-- Upgrade from 0.5.1 to 1.0.0

-- Verify loaded library matches this SQL script version
DO $$
DECLARE
    lib_ver text;
BEGIN
    lib_ver := pg_catalog.current_setting('pg_textsearch.library_version', true);
    IF lib_ver IS NULL THEN
        RAISE EXCEPTION
            'pg_textsearch library not loaded. '
            'Add pg_textsearch to shared_preload_libraries and restart.';
    END IF;
    IF lib_ver OPERATOR(pg_catalog.<>) '1.0.0' THEN
        RAISE EXCEPTION
            'pg_textsearch library version mismatch: loaded=%, expected=%. '
            'Restart the server after installing the new binary.',
            lib_ver, '1.0.0';
    END IF;
END $$;

-- New function: force-merge all segments into one
CREATE FUNCTION bm25_force_merge(index_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'tp_force_merge'
LANGUAGE C VOLATILE STRICT;

-- Revoke public execute on debug functions (superuser-only).
REVOKE EXECUTE ON FUNCTION bm25_dump_index(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_summarize_index(text) FROM PUBLIC;

-- Drop file-writing debug functions (moved behind compile-time flag).
DROP FUNCTION IF EXISTS bm25_dump_index(text, text);
DROP FUNCTION IF EXISTS bm25_debug_pageviz(text, text);

DO $$
BEGIN
    RAISE INFO 'pg_textsearch v1.0.0';
END
$$;
