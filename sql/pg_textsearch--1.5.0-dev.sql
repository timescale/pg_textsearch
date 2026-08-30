-- pg_textsearch extension version 1.5.0-dev

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_textsearch" to load this file. \quit

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
    IF lib_ver OPERATOR(pg_catalog.<>) '1.5.0-dev' THEN
        RAISE EXCEPTION
            'pg_textsearch library version mismatch: loaded=%, expected=%. '
            'Restart the server after installing the new binary.',
            lib_ver, '1.5.0-dev';
    END IF;
END $$;

-- Access method

CREATE FUNCTION @extschema@.tp_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'tp_handler'
LANGUAGE C;

CREATE ACCESS METHOD bm25 TYPE INDEX HANDLER @extschema@.tp_handler;

-- bm25vector type

CREATE FUNCTION @extschema@.bm25vector_in(cstring)
RETURNS @extschema@.bm25vector
AS 'MODULE_PATHNAME', 'tpvector_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.bm25vector_out(@extschema@.bm25vector)
RETURNS cstring
AS 'MODULE_PATHNAME', 'tpvector_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.bm25vector_recv(internal)
RETURNS @extschema@.bm25vector
AS 'MODULE_PATHNAME', 'tpvector_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.bm25vector_send(@extschema@.bm25vector)
RETURNS bytea
AS 'MODULE_PATHNAME', 'tpvector_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE @extschema@.bm25vector (
    INPUT = @extschema@.bm25vector_in,
    OUTPUT = @extschema@.bm25vector_out,
    RECEIVE = @extschema@.bm25vector_recv,
    SEND = @extschema@.bm25vector_send,
    STORAGE = extended,
    ALIGNMENT = int4
);

-- bm25query type

CREATE FUNCTION @extschema@.bm25query_in(cstring)
RETURNS @extschema@.bm25query
AS 'MODULE_PATHNAME', 'tpquery_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.bm25query_out(@extschema@.bm25query)
RETURNS cstring
AS 'MODULE_PATHNAME', 'tpquery_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.bm25query_recv(internal)
RETURNS @extschema@.bm25query
AS 'MODULE_PATHNAME', 'tpquery_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.bm25query_send(@extschema@.bm25query)
RETURNS bytea
AS 'MODULE_PATHNAME', 'tpquery_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE @extschema@.bm25query (
    INPUT = @extschema@.bm25query_in,
    OUTPUT = @extschema@.bm25query_out,
    RECEIVE = @extschema@.bm25query_recv,
    SEND = @extschema@.bm25query_send,
    STORAGE = extended,
    ALIGNMENT = int4
);

-- Convert text to bm25query
CREATE FUNCTION @extschema@.to_bm25query(input_text text)
RETURNS @extschema@.bm25query
AS 'MODULE_PATHNAME', 'to_tpquery_text'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION @extschema@.to_bm25query(input_text text, index_name text)
RETURNS @extschema@.bm25query
AS 'MODULE_PATHNAME', 'to_tpquery_text_index'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;


-- Equality function: bm25vector = bm25vector → boolean
CREATE FUNCTION @extschema@.bm25vector_eq(@extschema@.bm25vector, @extschema@.bm25vector)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tpvector_eq'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Create the = operator for equality
CREATE OPERATOR @extschema@.= (
    LEFTARG = @extschema@.bm25vector,
    RIGHTARG = @extschema@.bm25vector,
    FUNCTION = @extschema@.bm25vector_eq,
    COMMUTATOR = OPERATOR(@extschema@.=),
    HASHES
);


-- BM25 scoring function for text <@> bm25query operations
--
-- COST 1000: Standalone scoring is expensive. Each call parses document text
-- with to_tsvector (~14μs per doc), opens the index, looks up IDF values, and
-- calculates BM25 scores. High cost helps planner prefer index scans.
--
-- PARALLEL UNSAFE: standalone scoring opens the index relation by name,
-- attaches per-backend state (registry, DSA, LWLocks), and walks the
-- on-disk memtable chain under shared latches.  Parallel workers attempting
-- the same setup do not survive worker startup reliably (see follow-up
-- issue).  Ranked queries should use ORDER BY <@> ... LIMIT n, which is an
-- index scan and does not exercise this function in workers.
-- STABLE, not IMMUTABLE: the returned score depends on corpus statistics
-- that change as the indexed table is modified, so the result is only
-- stable within a single scan.  STABLE also keeps the planner from
-- constant-folding a fully-constant scoring expression at plan time, so the
-- runtime privilege check on the named index always runs under the invoking
-- role.
CREATE FUNCTION @extschema@.bm25_text_bm25query_score(left_text text, right_query @extschema@.bm25query)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_text_bm25query_score'
LANGUAGE C STABLE STRICT PARALLEL UNSAFE COST 1000;

-- bm25query equality function
CREATE FUNCTION @extschema@.bm25query_eq(@extschema@.bm25query, @extschema@.bm25query)
RETURNS boolean
AS 'MODULE_PATHNAME', 'tpquery_eq'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;



-- <@> operator for text <@> bm25query operations
CREATE OPERATOR @extschema@.<@> (
    LEFTARG = text,
    RIGHTARG = @extschema@.bm25query,
    PROCEDURE = @extschema@.bm25_text_bm25query_score
);

-- Function for text <@> text operator (planner hook rewrites to text <@> bm25query)
-- COST 1000: High cost makes planner prefer index scans over seq scan + sort.
-- In practice, this function errors without index scan context, but the cost
-- helps the planner choose the right path before execution.
CREATE FUNCTION @extschema@.bm25_text_text_score(text, text) RETURNS float8
    AS 'MODULE_PATHNAME', 'bm25_text_text_score'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- Stub function returning cached score from index scan.
-- The planner hook replaces resjunk ORDER BY expressions with calls to this
-- function, avoiding expensive re-computation of BM25 scores.
CREATE FUNCTION @extschema@.bm25_get_current_score() RETURNS float8
    AS 'MODULE_PATHNAME', 'bm25_get_current_score'
    LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

-- <@> operator for text <@> text operations (implicit index resolution)
-- The planner hook transforms this to text <@> bm25query when a BM25 index exists
CREATE OPERATOR @extschema@.<@> (
    LEFTARG = text,
    RIGHTARG = text,
    PROCEDURE = @extschema@.bm25_text_text_score
);

-- = operator for bm25query equality
CREATE OPERATOR @extschema@.= (
    LEFTARG = @extschema@.bm25query,
    RIGHTARG = @extschema@.bm25query,
    FUNCTION = @extschema@.bm25query_eq,
    COMMUTATOR = OPERATOR(@extschema@.=),
    HASHES
);

-- bm25 operator class for text columns
-- The planner hook rewrites text <@> text to text <@> bm25query, so we only
-- need to register the bm25query operator and support function here.
CREATE OPERATOR CLASS @extschema@.text_bm25_ops
DEFAULT FOR TYPE text USING bm25 AS
    OPERATOR    1   @extschema@.<@> (text, @extschema@.bm25query) FOR ORDER BY float_ops,
    FUNCTION    8   (text, @extschema@.bm25query)   @extschema@.bm25_text_bm25query_score(text, @extschema@.bm25query);

-- BM25 scoring function for text[] <@> bm25query operations
-- Flattens array elements with spaces, then scores as a single document.
-- PARALLEL UNSAFE: delegates to bm25_text_bm25query_score; see that
-- function's declaration for the rationale.
CREATE FUNCTION @extschema@.bm25_textarray_bm25query_score(
    left_arr text[], right_query @extschema@.bm25query)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_textarray_bm25query_score'
LANGUAGE C STABLE STRICT PARALLEL UNSAFE COST 1000;

-- Error stub for text[] <@> text (planner should rewrite to bm25query)
CREATE FUNCTION @extschema@.bm25_textarray_text_score(text[], text)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_textarray_text_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- <@> operator for text[] <@> bm25query operations
CREATE OPERATOR @extschema@.<@> (
    LEFTARG = text[],
    RIGHTARG = @extschema@.bm25query,
    PROCEDURE = @extschema@.bm25_textarray_bm25query_score
);

-- <@> operator for text[] <@> text operations (implicit index resolution)
CREATE OPERATOR @extschema@.<@> (
    LEFTARG = text[],
    RIGHTARG = text,
    PROCEDURE = @extschema@.bm25_textarray_text_score
);

-- bm25 operator class for text[] columns
CREATE OPERATOR CLASS @extschema@.text_array_bm25_ops
DEFAULT FOR TYPE text[] USING bm25 AS
    OPERATOR    1   @extschema@.<@> (text[], @extschema@.bm25query)
                    FOR ORDER BY float_ops,
    FUNCTION    8   (text[], @extschema@.bm25query)
                    @extschema@.bm25_textarray_bm25query_score(
                        text[], @extschema@.bm25query);

-- Debug function to dump index contents (memtable and segments)
CREATE FUNCTION @extschema@.bm25_dump_index(text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_dump_index'
    LANGUAGE C STRICT STABLE;

-- Function to force segment write (spill memtable to disk)
CREATE FUNCTION @extschema@.bm25_spill_index(index_name text)
RETURNS int4
AS 'MODULE_PATHNAME', 'tp_spill_memtable'
LANGUAGE C VOLATILE STRICT;

-- One-shot, size-bounded copy-on-write segment compaction
CREATE FUNCTION @extschema@.bm25_force_merge(index_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'tp_force_merge'
LANGUAGE C VOLATILE STRICT;

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

-- Named default privileges are copied onto new functions and survive a
-- PUBLIC-only revoke. Keep these worker helpers owner-only until the
-- operator explicitly grants the dedicated compactor role.
DO $$
DECLARE
    helper record;
BEGIN
    FOR helper IN
        SELECT namespace.nspname,
               procedure.proname,
               grantee.rolname AS grantee_name
        FROM pg_catalog.pg_extension extension
             JOIN pg_catalog.pg_depend dependency
               ON dependency.refclassid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_extension'::pg_catalog.regclass
              AND dependency.refobjid OPERATOR(pg_catalog.=) extension.oid
              AND dependency.classid OPERATOR(pg_catalog.=)
                      'pg_catalog.pg_proc'::pg_catalog.regclass
              AND dependency.deptype OPERATOR(pg_catalog.=) 'e'
             JOIN pg_catalog.pg_proc procedure
               ON procedure.oid OPERATOR(pg_catalog.=) dependency.objid
             JOIN pg_catalog.pg_namespace namespace
               ON namespace.oid OPERATOR(pg_catalog.=)
                      procedure.pronamespace
             CROSS JOIN LATERAL pg_catalog.aclexplode(
                 coalesce(
                     procedure.proacl,
                     pg_catalog.acldefault(
                         'f', procedure.proowner))) acl
             JOIN pg_catalog.pg_roles grantee
               ON grantee.oid OPERATOR(pg_catalog.=) acl.grantee
        WHERE extension.extname OPERATOR(pg_catalog.=) 'pg_textsearch'
          AND procedure.proname OPERATOR(pg_catalog.=) ANY (ARRAY[
                  'bm25_compact_step_if_current',
                  'bm25_needs_compaction_if_current'])
          AND procedure.pronargs OPERATOR(pg_catalog.=) 4
          AND acl.grantee OPERATOR(pg_catalog.<>) procedure.proowner
    LOOP
        EXECUTE pg_catalog.format(
            'REVOKE ALL ON FUNCTION %I.%I('
            'pg_catalog.oid, pg_catalog.oid, pg_catalog.oid, pg_catalog.oid'
            ') FROM %I CASCADE',
            helper.nspname,
            helper.proname,
            helper.grantee_name);
    END LOOP;
END
$$;

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

-- Fast summary function showing only statistics (no content dump)
CREATE FUNCTION @extschema@.bm25_summarize_index(text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_summarize_index'
    LANGUAGE C STRICT STABLE;

CREATE FUNCTION @extschema@.bm25_pending_free_pages(index_name text)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'tp_pending_free_pages'
    LANGUAGE C STRICT STABLE;

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

-- Revoke public execute on debug functions (superuser-only).
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_dump_index(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_summarize_index(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_pending_free_pages(text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION
    @extschema@.bm25_test_recycle_tombstone_head(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION
    @extschema@.bm25_test_corrupt_tombstone_head(text) FROM PUBLIC;

-- The bm25_test_memtable_page / bm25_test_memtable_append /
-- bm25_test_chain_source / bm25_memtable_chain /
-- bm25_memtable_dead_pages functions are
-- INTERNAL-ONLY test scaffolds for the on-disk memtable redesign
-- (issue #374).  They are not part of the supported
-- public API: their signatures, return values, and existence are
-- subject to change or removal in ANY release (including patch
-- releases) without notice or upgrade paths.  Do not depend on
-- them from application code.
CREATE FUNCTION @extschema@.bm25_test_memtable_page(case_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_test_memtable_page'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.bm25_test_memtable_append(
    index_name text, case_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_test_memtable_append'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.bm25_memtable_chain(
    index_name text,
    OUT blkno bigint,
    OUT n_records integer,
    OUT free_offset integer,
    OUT next_block bigint,
    OUT flags integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'bm25_memtable_chain'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.bm25_memtable_dead_pages(
    index_name text,
    OUT blkno bigint,
    OUT flags integer,
    OUT dead_fxid bigint,
    OUT n_records integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'bm25_memtable_dead_pages'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.bm25_test_chain_source(
    index_name text, case_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_test_chain_source'
LANGUAGE C STRICT;

-- Cache apply protocol scaffolds (in-memory memtable cache).
-- Same INTERNAL-ONLY disclaimer as the v2 chain scaffolds above.
-- Each returns a (result, records_applied, cursor_seq,
-- estimated_bytes) tuple.
CREATE FUNCTION @extschema@.bm25_cache_cold_build(
    index_name text,
    OUT result text,
    OUT records_applied bigint,
    OUT cursor_seq bigint,
    OUT estimated_bytes bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'bm25_cache_cold_build'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.bm25_cache_apply_to_tail(
    index_name text,
    OUT result text,
    OUT records_applied bigint,
    OUT cursor_seq bigint,
    OUT estimated_bytes bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'bm25_cache_apply_to_tail'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.bm25_cache_bump_spill_generation(
    index_name text)
RETURNS bigint
AS 'MODULE_PATHNAME', 'bm25_cache_bump_spill_generation'
LANGUAGE C STRICT;

-- Cache memory-cap scaffolds.  Same INTERNAL-ONLY disclaimer as
-- above.  See docs/memtable_cache.md §"Memory cap (3 tiers)".
CREATE FUNCTION @extschema@.bm25_cache_global_estimated_bytes()
RETURNS bigint
AS 'MODULE_PATHNAME', 'bm25_cache_global_estimated_bytes'
LANGUAGE C STRICT;

CREATE FUNCTION @extschema@.bm25_cache_evict_largest(index_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_cache_evict_largest'
LANGUAGE C STRICT;

-- Cache source scaffold (in-memory memtable cache).
-- Same INTERNAL-ONLY disclaimer as above.
CREATE FUNCTION @extschema@.bm25_test_cache_source(
    index_name text, case_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'bm25_test_cache_source'
LANGUAGE C STRICT;

REVOKE EXECUTE ON FUNCTION @extschema@.bm25_test_memtable_page(text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_test_memtable_append(text, text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_memtable_chain(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_memtable_dead_pages(text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_test_chain_source(text, text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_cache_cold_build(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_cache_apply_to_tail(text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_cache_bump_spill_generation(text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_test_cache_source(text, text)
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_cache_global_estimated_bytes()
    FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_cache_evict_largest(text)
    FROM PUBLIC;
