-- Upgrade from 0.6.0 to 1.0.0-dev
-- No schema changes (bm25_force_merge already exists in 0.6.0)

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

-- Revoke public execute on administrative and debug functions
REVOKE EXECUTE ON FUNCTION bm25_spill_index(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_force_merge(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_dump_index(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_dump_index(text, text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_summarize_index(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_debug_pageviz(text, text) FROM PUBLIC;

DO $$
BEGIN
    RAISE WARNING 'pg_textsearch v1.0.0-dev is a prerelease. Do not use in production.';
END
$$;
