-- Upgrade from 0.5.0 to 0.5.1
-- No schema changes

DO $$
BEGIN
    RAISE WARNING 'pg_textsearch v0.5.1 is a prerelease. Do not use in production.';
END
$$;
