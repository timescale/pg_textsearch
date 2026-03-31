-- Upgrade from 0.6.0 to 0.6.1
-- No schema changes

-- Verify pg_textsearch library is loaded
DO $$
BEGIN
    IF pg_catalog.current_setting('pg_textsearch.library_version', true)
       IS NULL
    THEN
        RAISE EXCEPTION
            'pg_textsearch library not loaded. '
            'Add pg_textsearch to shared_preload_libraries and restart.';
    END IF;
END $$;

DO $$
BEGIN
    RAISE INFO 'pg_textsearch: upgraded to v0.6.1';
END
$$;
