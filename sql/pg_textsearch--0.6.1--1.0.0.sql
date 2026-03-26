-- Upgrade from 0.6.1 to 1.0.0
-- No schema changes

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

DO $$
BEGIN
    RAISE INFO 'pg_textsearch v1.0.0';
END
$$;
