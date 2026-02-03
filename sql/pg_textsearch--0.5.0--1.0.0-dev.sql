-- Upgrade from 0.5.0 to 1.0.0-dev
-- No schema changes

DO $$
BEGIN
    RAISE WARNING 'pg_textsearch v1.0.0-dev is a prerelease. Do not use in production.';
END
$$;
