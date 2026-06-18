-- Upgrade from 1.3.0 to 1.4.0-dev

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

-- Deferred, standby-safe reclaim of displaced segment pages (#380):
-- count the pages currently parked in the tombstone chain awaiting
-- horizon-gated FSM reclaim.  Superuser-only, like the other debug
-- introspection functions.
CREATE FUNCTION bm25_pending_free_pages(index_name text)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'tp_pending_free_pages'
    LANGUAGE C STRICT STABLE;

REVOKE EXECUTE ON FUNCTION bm25_pending_free_pages(text) FROM PUBLIC;
