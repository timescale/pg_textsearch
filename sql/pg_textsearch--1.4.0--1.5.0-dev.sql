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

COMMENT ON FUNCTION @extschema@.bm25_force_merge(text) IS
    'Run one-shot copy-on-write compaction into the fewest conservatively size-bounded segments; existing over-budget singletons remain uncombinable, and displaced pages enter deferred reclaim.';

-- PARALLEL RESTRICTED: opens the index relation, which may be a local
-- temporary index whose buffers a parallel worker cannot reach.
CREATE FUNCTION @extschema@.bm25_level_counts(idx regclass)
RETURNS int[]
AS 'MODULE_PATHNAME', 'tp_level_counts'
LANGUAGE C VOLATILE STRICT PARALLEL RESTRICTED;

CREATE FUNCTION @extschema@.bm25_compact(idx regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'tp_compact_index'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION @extschema@.bm25_compact(regclass) IS
    'Run threshold compaction to completion under one per-index exclusive lock. Passes already published are not undone by ROLLBACK, so a cascade that errors partway leaves its earlier passes applied.';

CREATE FUNCTION @extschema@.bm25_compact_step(idx regclass)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tp_compact_index_step'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION @extschema@.bm25_compact_step(regclass) IS
    'Run at most one compaction pass and report whether one ran, letting a caller spread a cascade over several transactions. A published pass is not undone by ROLLBACK.';

-- VOLATILE because it reads live metapage state, and PARALLEL
-- RESTRICTED to match bm25_level_counts.  Every level counts: the top
-- level compacts into itself, so its debt is reducible like any
-- other's.
CREATE FUNCTION @extschema@.bm25_needs_compaction(idx regclass)
RETURNS boolean
LANGUAGE sql VOLATILE STRICT PARALLEL RESTRICTED
SET search_path = pg_catalog, pg_temp
AS $$
    SELECT EXISTS (
        SELECT 1
        FROM pg_catalog.unnest(
                 @extschema@.bm25_level_counts(idx)) AS cnt
        WHERE cnt >= pg_catalog.current_setting(
                  'pg_textsearch.segments_per_level')::int
    );
$$;

COMMENT ON FUNCTION @extschema@.bm25_needs_compaction(regclass) IS
    'Report whether any level holds at least segments_per_level segments. Advisory only: a level whose segments are all over budget is reported as full even though bm25_compact_step has no way to reduce it, so this must not be used on its own as a retry condition.';
