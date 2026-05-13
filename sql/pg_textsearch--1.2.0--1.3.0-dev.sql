-- Upgrade from 1.2.0 to 1.3.0-dev

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

-- Internal-only scaffold: exercises the on-disk memtable page format
-- helpers introduced in Phase 1 of the memtable v2 redesign (issue
-- #374). Subject to removal once later phases provide end-to-end
-- coverage of the same code paths.
CREATE FUNCTION bm25_test_memtable_page(case_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_test_memtable_page'
LANGUAGE C STRICT;
