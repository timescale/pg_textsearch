-- Upgrade from 0.4.0 to 0.4.1
-- This is a bugfix release with no SQL changes.

DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded to v0.4.1';
END;
$$;
