-- Upgrade from 1.4.0 to 1.5.0-dev

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

CREATE FUNCTION @extschema@.bm25_level_counts(idx regclass)
RETURNS int[]
AS 'MODULE_PATHNAME', 'tp_level_counts'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.bm25_compact(idx regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'tp_compact_index'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION @extschema@.bm25_compact_step(idx regclass)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tp_compact_index_step'
LANGUAGE C VOLATILE STRICT;
