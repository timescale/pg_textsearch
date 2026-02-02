-- Upgrade from 0.4.2 to 0.5.0
-- No schema changes

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.5.0';
END
$$;
