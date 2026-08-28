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

CREATE FUNCTION @extschema@.bm25_compact_step_if_current(
    index_oid oid,
    database_oid oid,
    tablespace_oid oid,
    relfilenumber oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tp_compact_index_step_if_current'
LANGUAGE C VOLATILE STRICT;

CREATE FUNCTION @extschema@.bm25_needs_compaction_if_current(
    index_oid oid,
    database_oid oid,
    tablespace_oid oid,
    relfilenumber oid)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tp_needs_compaction_if_current'
LANGUAGE C STABLE STRICT;

REVOKE ALL ON FUNCTION
    @extschema@.bm25_compact_step_if_current(oid, oid, oid, oid),
    @extschema@.bm25_needs_compaction_if_current(oid, oid, oid, oid)
FROM PUBLIC;

CREATE FUNCTION @extschema@.bm25_needs_compaction(idx regclass)
RETURNS boolean
LANGUAGE sql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT EXISTS (
        SELECT 1
        FROM pg_catalog.unnest(@extschema@.bm25_level_counts(idx))
             WITH ORDINALITY AS t(cnt, lvl)
        WHERE
          -- TP_MAX_LEVELS - 1; ordinality 8 is the ineligible top level.
          t.lvl <= 7
          AND t.cnt >= pg_catalog.current_setting(
                  'pg_textsearch.segments_per_level')::int
    );
$$;

CREATE FUNCTION @extschema@.bm25_indexes_needing_compaction()
RETURNS SETOF regclass
LANGUAGE sql STABLE
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT c.oid::pg_catalog.regclass
    FROM pg_catalog.pg_class c
         JOIN pg_catalog.pg_am am ON am.oid = c.relam
         JOIN pg_catalog.pg_index i ON i.indexrelid = c.oid
    WHERE am.amname = 'bm25'
      AND c.relkind = 'i'
      AND c.relpersistence <> 't'
      AND i.indisvalid
      AND i.indisready
      AND pg_catalog.pg_has_role(c.relowner, 'USAGE')
    ORDER BY c.oid;
$$;

CREATE FUNCTION @extschema@.bm25_compact_pending()
RETURNS integer
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $$
DECLARE
    idx regclass;
    n   integer := 0;
BEGIN
    FOR idx IN
        SELECT * FROM @extschema@.bm25_indexes_needing_compaction()
    LOOP
        BEGIN
            IF @extschema@.bm25_needs_compaction(idx) THEN
                PERFORM @extschema@.bm25_compact(idx);
                n := n + 1;
            END IF;
        EXCEPTION WHEN OTHERS THEN
            RAISE WARNING 'bm25: compaction of % failed: %', idx, SQLERRM;
        END;
    END LOOP;

    RETURN n;
END;
$$;
