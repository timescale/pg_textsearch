-- Upgrade from 1.3.1 to 1.4.0-dev

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

-- Re-declare the standalone scoring functions as STABLE (previously
-- IMMUTABLE). Their result depends on corpus statistics that change with
-- the indexed table, and STABLE prevents plan-time constant folding so the
-- runtime privilege check on the named index always runs under the invoking
-- role.
ALTER FUNCTION @extschema@.bm25_text_bm25query_score(text, @extschema@.bm25query)
    STABLE;
ALTER FUNCTION @extschema@.bm25_textarray_bm25query_score(text[], @extschema@.bm25query)
    STABLE;
