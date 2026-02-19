-- Upgrade from 0.5.0 to 0.5.1
-- Bug fixes only, no schema changes

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.5.1';
END
$$;
