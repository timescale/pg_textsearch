-- pg_textsearch extension version 1.4.0-dev

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
    IF lib_ver OPERATOR(pg_catalog.<>) '1.4.0-dev' THEN
        RAISE EXCEPTION
            'pg_textsearch library version mismatch: loaded=%, expected=%. '
            'Restart the server after installing the new binary.',
            lib_ver, '1.4.0-dev';
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
CREATE FUNCTION @extschema@.bm25_text_bm25query_score(left_text text, right_query @extschema@.bm25query)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_text_bm25query_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL UNSAFE COST 1000;

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
LANGUAGE C IMMUTABLE STRICT PARALLEL UNSAFE COST 1000;

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

-- Force-merge all segments into one, à la Lucene's forceMerge(1)
CREATE FUNCTION @extschema@.bm25_force_merge(index_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'tp_force_merge'
LANGUAGE C VOLATILE STRICT;

-- Fast summary function showing only statistics (no content dump)
CREATE FUNCTION @extschema@.bm25_summarize_index(text) RETURNS text
    AS 'MODULE_PATHNAME', 'tp_summarize_index'
    LANGUAGE C STRICT STABLE;

-- Revoke public execute on debug functions (superuser-only).
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_dump_index(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION @extschema@.bm25_summarize_index(text) FROM PUBLIC;

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
