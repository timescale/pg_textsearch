-- Upgrade from 0.6.0 to 0.6.1
-- Bugfix release: fix crash on ROLLBACK after error in transaction block
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
    IF lib_ver OPERATOR(pg_catalog.<>) '0.6.1' THEN
        RAISE EXCEPTION
            'pg_textsearch library version mismatch: loaded=%, expected=%. '
            'Restart the server after installing the new binary.',
            lib_ver, '0.6.1';
    END IF;
END $$;

DO $$
BEGIN
    RAISE WARNING 'pg_textsearch v0.6.1 is a prerelease. Do not use in production.';
END
$$;
