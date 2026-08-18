-- Upgrade from 1.3.1 to 1.4.0

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

-- INTERNAL-ONLY test scaffold (issues #426, #427): return the live
-- head tombstone page to the index FSM so the next allocator can pick
-- it up, reproducing the stale-FSM / non-atomic-claim page-reuse
-- hazard without an actual crash.  Superuser-only; not a supported API.
CREATE FUNCTION @extschema@.bm25_test_recycle_tombstone_head(
    index_name text)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'tp_test_recycle_tombstone_head'
    LANGUAGE C VOLATILE STRICT;

-- INTERNAL-ONLY test scaffold (issue #427): overwrite the head
-- tombstone page's magic so the chain node is corrupt, simulating a
-- page-reuse clobber, to exercise the drain's self-healing recovery.
-- Superuser-only; not a supported API.
CREATE FUNCTION @extschema@.bm25_test_corrupt_tombstone_head(
    index_name text)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'tp_test_corrupt_tombstone_head'
    LANGUAGE C VOLATILE STRICT;

REVOKE EXECUTE ON FUNCTION
    @extschema@.bm25_test_recycle_tombstone_head(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION
    @extschema@.bm25_test_corrupt_tombstone_head(text) FROM PUBLIC;
