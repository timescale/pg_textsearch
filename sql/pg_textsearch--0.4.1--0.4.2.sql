-- Upgrade from 0.4.1 to 0.4.2
-- No schema changes

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.4.2';
END
$$;
