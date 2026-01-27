-- Upgrade from 0.4.0 to 0.4.1
-- No schema changes

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.4.1';
END
$$;
