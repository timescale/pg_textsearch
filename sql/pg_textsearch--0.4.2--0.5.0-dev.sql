-- Upgrade from 0.4.2 to 0.5.0-dev
-- No schema changes

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.5.0-dev (prerelease)';
END
$$;
