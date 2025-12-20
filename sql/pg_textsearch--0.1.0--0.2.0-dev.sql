-- Upgrade from 0.1.0 to 0.2.0-dev
-- No schema changes - internal storage format improvements only
-- IMPORTANT: Requires REINDEX for existing indexes to use V2 segment format

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.2.0-dev';
    RAISE INFO 'Run REINDEX on bm25 indexes to use new V2 segment format';
END
$$;
