-- Upgrade from 1.0.0 to 1.1.0

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
    IF lib_ver OPERATOR(pg_catalog.<>) '1.1.0' THEN
        RAISE EXCEPTION
            'pg_textsearch library version mismatch: loaded=%, expected=%. '
            'Restart the server after installing the new binary.',
            lib_ver, '1.1.0';
    END IF;
END $$;

-- Native text[] support: scoring functions, operators, operator class.
-- Flattens array elements with spaces, then scores as a single document.
CREATE FUNCTION @extschema@.bm25_textarray_bm25query_score(
    left_arr text[], right_query @extschema@.bm25query)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_textarray_bm25query_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

-- Error stub for text[] <@> text (planner should rewrite to bm25query)
CREATE FUNCTION @extschema@.bm25_textarray_text_score(text[], text)
RETURNS float8
AS 'MODULE_PATHNAME', 'bm25_textarray_text_score'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE COST 1000;

CREATE OPERATOR @extschema@.<@> (
    LEFTARG = text[],
    RIGHTARG = @extschema@.bm25query,
    PROCEDURE = @extschema@.bm25_textarray_bm25query_score
);

CREATE OPERATOR @extschema@.<@> (
    LEFTARG = text[],
    RIGHTARG = text,
    PROCEDURE = @extschema@.bm25_textarray_text_score
);

CREATE OPERATOR CLASS @extschema@.text_array_bm25_ops
DEFAULT FOR TYPE text[] USING bm25 AS
    OPERATOR    1   @extschema@.<@> (text[], @extschema@.bm25query)
                    FOR ORDER BY float_ops,
    FUNCTION    8   (text[], @extschema@.bm25query)
                    @extschema@.bm25_textarray_bm25query_score(
                        text[], @extschema@.bm25query);

-- Memory usage visibility function.
-- Intentionally accessible to all users (monitoring/ops use case).
-- Only exposes aggregate DSA byte counts and configured limits,
-- not per-index or per-user data.
CREATE FUNCTION bm25_memory_usage(
    OUT dsa_total_bytes int8,
    OUT dsa_total_mb float4,
    OUT estimated_bytes int8,
    OUT estimated_mb float4,
    OUT counter_bytes int8,
    OUT memory_limit_mb float4,
    OUT usage_pct float4
)
AS 'MODULE_PATHNAME', 'tp_memory_usage'
LANGUAGE C VOLATILE;

DO $$
BEGIN
    RAISE INFO 'pg_textsearch v1.1.0';
END
$$;
