-- pg_textsearch extension upgrade from version 0.0.1 to 0.0.3
-- No schema changes required between these versions

-- Display upgrade message
DO $$
BEGIN
    RAISE INFO 'pg_textsearch upgraded from v0.0.1 to v0.0.3';
END
$$;
