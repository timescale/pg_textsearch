-- Upgrade from 0.5.0 to 1.0.0-dev

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
    IF lib_ver OPERATOR(pg_catalog.<>) '1.0.0-dev' THEN
        RAISE EXCEPTION
            'pg_textsearch library version mismatch: loaded=%, expected=%. '
            'Restart the server after installing the new binary.',
            lib_ver, '1.0.0-dev';
    END IF;
END $$;

-- New function: force-merge all segments into one
CREATE FUNCTION bm25_force_merge(index_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'tp_force_merge'
LANGUAGE C VOLATILE STRICT;

-- Revoke public execute on debug functions (superuser-only).
-- bm25_debug_pageviz may not exist when upgrading from pre-0.5.0
-- (it was added in the 0.5.0 base install but not in upgrade scripts),
-- so revoke it conditionally.
REVOKE EXECUTE ON FUNCTION bm25_dump_index(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_dump_index(text, text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_summarize_index(text) FROM PUBLIC;
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_proc WHERE proname = 'bm25_debug_pageviz') THEN
        EXECUTE 'REVOKE EXECUTE ON FUNCTION bm25_debug_pageviz(text, text) FROM PUBLIC';
    END IF;
END $$;

DO $$
BEGIN
    RAISE WARNING 'pg_textsearch v1.0.0-dev is a prerelease. Do not use in production.';
END
$$;
