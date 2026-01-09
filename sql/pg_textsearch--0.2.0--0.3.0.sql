-- Upgrade from 0.2.0 to 0.3.0
-- No schema changes

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.3.0';
END
$$;
