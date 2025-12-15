-- Upgrade from 0.1.0 to 0.1.1-dev
-- No schema changes in this version

-- Display warning about prerelease status
DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.1.1-dev';
END
$$;
