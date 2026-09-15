-- BM25/RLS combinations are allowed by default, but administrators can
-- disable creation, rebuild, or later RLS enablement.

CREATE EXTENSION pg_textsearch;

SHOW pg_textsearch.allow_rls;

CREATE TABLE rls_existing (id integer, content text);
INSERT INTO rls_existing VALUES (1, 'known secret term');
ALTER TABLE rls_existing ENABLE ROW LEVEL SECURITY;
CREATE INDEX rls_existing_idx ON rls_existing USING bm25(content)
    WITH (text_config='english');

SET pg_textsearch.allow_rls = off;

ALTER TABLE IF EXISTS rls_missing ENABLE ROW LEVEL SECURITY;

-- Existing combinations remain usable when the GUC changes.
SELECT content
FROM rls_existing
ORDER BY content <@> to_bm25query('known', 'rls_existing_idx')
LIMIT 1;
INSERT INTO rls_existing VALUES (2, 'another known term');

\set VERBOSITY terse
REINDEX INDEX rls_existing_idx;
\set VERBOSITY default

CREATE TABLE rls_before_index (id integer, content text);
ALTER TABLE rls_before_index ENABLE ROW LEVEL SECURITY;
\set VERBOSITY terse
BEGIN;
CREATE INDEX CONCURRENTLY rls_concurrent_idx
    ON rls_before_index USING bm25(content)
    WITH (text_config='english');
ROLLBACK;
CREATE INDEX CONCURRENTLY rls_concurrent_top_idx
    ON rls_before_index USING bm25(content)
    WITH (text_config='english');
\pset format unaligned
SELECT to_regclass('rls_concurrent_top_idx') IS NULL
    AS no_index_catalog_entry;
\pset format aligned
CREATE INDEX rls_before_index_idx ON rls_before_index USING bm25(content)
    WITH (text_config='english');
\set VERBOSITY default

CREATE TABLE index_before_rls (id integer, content text);
CREATE INDEX index_before_rls_idx ON index_before_rls USING bm25(content)
    WITH (text_config='english');
\set VERBOSITY terse
ALTER VIEW index_before_rls ENABLE ROW LEVEL SECURITY;
ALTER TABLE index_before_rls ENABLE ROW LEVEL SECURITY;
\set VERBOSITY default

RESET pg_textsearch.allow_rls;
ALTER TABLE index_before_rls ENABLE ROW LEVEL SECURITY;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'rls_guc_user') THEN
        EXECUTE 'REASSIGN OWNED BY rls_guc_user TO CURRENT_USER';
        EXECUTE 'DROP OWNED BY rls_guc_user CASCADE';
        DROP ROLE rls_guc_user;
    END IF;
END $$;
CREATE ROLE rls_guc_user;
SET pg_textsearch.allow_rls = off;
SET ROLE rls_guc_user;
\set VERBOSITY terse
CREATE INDEX rls_unauthorized_idx ON rls_before_index USING bm25(content)
    WITH (text_config='english');
ALTER TABLE index_before_rls ENABLE ROW LEVEL SECURITY;
SET pg_textsearch.allow_rls = off;
\set VERBOSITY default
RESET ROLE;

DROP OWNED BY rls_guc_user;
DROP ROLE rls_guc_user;
DROP TABLE rls_existing, rls_before_index, index_before_rls CASCADE;

CREATE TABLE rls_without_extension (id integer);
SET pg_textsearch.allow_rls = off;
DROP EXTENSION pg_textsearch;
ALTER TABLE rls_without_extension ENABLE ROW LEVEL SECURITY;
DROP TABLE rls_without_extension;
