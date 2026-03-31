-- Upgrade from 0.6.1 to 1.0.0
-- No schema changes

-- Verify pg_textsearch library is loaded
DO $$
BEGIN
    IF pg_catalog.current_setting('pg_textsearch.library_version', true)
       IS NULL
    THEN
        RAISE EXCEPTION
            'pg_textsearch library not loaded. '
            'Add pg_textsearch to shared_preload_libraries and restart.';
    END IF;
END $$;

-- Revoke public execute on debug functions (superuser-only).
REVOKE EXECUTE ON FUNCTION bm25_dump_index(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION bm25_summarize_index(text) FROM PUBLIC;

-- Drop file-writing debug functions (moved behind compile-time flag).
DROP FUNCTION IF EXISTS bm25_dump_index(text, text);
DROP FUNCTION IF EXISTS bm25_debug_pageviz(text, text);

DO $$
BEGIN
    RAISE INFO 'pg_textsearch v1.0.0';
END
$$;
