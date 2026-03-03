-- Upgrade from 0.5.1 to 0.6.0

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
    IF lib_ver OPERATOR(pg_catalog.<>) '0.6.0' THEN
        RAISE EXCEPTION
            'pg_textsearch library version mismatch: loaded=%, expected=%. '
            'Restart the server after installing the new binary.',
            lib_ver, '0.6.0';
    END IF;
END $$;

-- New function: force-merge all segments into one
CREATE FUNCTION bm25_force_merge(index_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'tp_force_merge'
LANGUAGE C VOLATILE STRICT;

-- Segment format changed from V3 to V4 (uint32 to uint64 offsets).
-- Existing indexes must be rebuilt to use the new format.
DO $$
BEGIN
    RAISE NOTICE 'pg_textsearch upgraded to v0.6.0. '
        'Segment format changed (V3 → V4). '
        'Run REINDEX on all bm25 indexes to rebuild with the new format.';
END
$$;
