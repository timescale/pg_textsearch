-- Upgrade from 0.3.0 to 0.4.0-dev
-- No schema changes

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.4.0-dev';
END
$$;
