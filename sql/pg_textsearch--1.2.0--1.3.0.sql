-- Upgrade from 1.2.0 to 1.3.0

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

-- The bm25_test_memtable_page / bm25_test_memtable_append /
-- bm25_test_chain_source / bm25_memtable_chain /
-- bm25_memtable_dead_pages functions are
-- INTERNAL-ONLY test scaffolds for the on-disk memtable redesign
-- (issue #374).  Not part of the supported public API.
-- See pg_textsearch--1.3.0.sql for the full disclaimer.
CREATE FUNCTION bm25_test_memtable_page(case_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_test_memtable_page'
LANGUAGE C STRICT;

CREATE FUNCTION bm25_test_memtable_append(
    index_name text, case_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_test_memtable_append'
LANGUAGE C STRICT;

CREATE FUNCTION bm25_memtable_chain(
    index_name text,
    OUT blkno bigint,
    OUT n_records integer,
    OUT free_offset integer,
    OUT next_block bigint,
    OUT flags integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'bm25_memtable_chain'
LANGUAGE C STRICT;

CREATE FUNCTION bm25_memtable_dead_pages(
    index_name text,
    OUT blkno bigint,
    OUT flags integer,
    OUT dead_fxid bigint,
    OUT n_records integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'bm25_memtable_dead_pages'
LANGUAGE C STRICT;

CREATE FUNCTION bm25_test_chain_source(
    index_name text, case_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_test_chain_source'
LANGUAGE C STRICT;

CREATE FUNCTION bm25_cache_cold_build(
    index_name text,
    OUT result text,
    OUT records_applied bigint,
    OUT cursor_seq bigint,
    OUT estimated_bytes bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'bm25_cache_cold_build'
LANGUAGE C STRICT;

CREATE FUNCTION bm25_cache_apply_to_tail(
    index_name text,
    OUT result text,
    OUT records_applied bigint,
    OUT cursor_seq bigint,
    OUT estimated_bytes bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'bm25_cache_apply_to_tail'
LANGUAGE C STRICT;

CREATE FUNCTION bm25_cache_bump_spill_generation(index_name text)
RETURNS bigint
AS 'MODULE_PATHNAME', 'bm25_cache_bump_spill_generation'
LANGUAGE C STRICT;

-- Cache memory-cap scaffolds.
CREATE FUNCTION bm25_cache_global_estimated_bytes()
RETURNS bigint
AS 'MODULE_PATHNAME', 'bm25_cache_global_estimated_bytes'
LANGUAGE C STRICT;

CREATE FUNCTION bm25_cache_evict_largest(index_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_cache_evict_largest'
LANGUAGE C STRICT;

-- Cache source scaffold.
CREATE FUNCTION bm25_test_cache_source(
    index_name text, case_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_test_cache_source'
LANGUAGE C STRICT;

REVOKE EXECUTE ON FUNCTION bm25_test_memtable_page(text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_test_memtable_append(text, text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_memtable_chain(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_memtable_dead_pages(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_test_chain_source(text, text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_cache_cold_build(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_cache_apply_to_tail(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_cache_bump_spill_generation(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_test_cache_source(text, text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_cache_global_estimated_bytes() FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_cache_evict_largest(text) FROM PUBLIC;

-- The on-disk memtable redesign (issue #374) makes the soft-limit
-- memory_usage SRF obsolete along with the underlying soft-limit
-- machinery; the chain_page_count-based auto-spill heuristic replaces
-- the old global byte accounting.
DROP FUNCTION IF EXISTS bm25_memory_usage();

-- Standalone scoring functions are not parallel-safe: they open the index
-- relation by name, attach per-backend state, and walk the on-disk memtable
-- chain under shared latches.  Parallel workers attempting the same setup
-- do not survive worker startup reliably.  Ranked queries should use
-- ORDER BY <@> ... LIMIT n, which uses an index scan and does not exercise
-- these functions in workers.
ALTER FUNCTION bm25_text_bm25query_score(text, bm25query) PARALLEL UNSAFE;
ALTER FUNCTION bm25_textarray_bm25query_score(text[], bm25query)
    PARALLEL UNSAFE;
