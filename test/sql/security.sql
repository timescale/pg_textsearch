-- Test security restrictions on debug and admin functions

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test data
CREATE TABLE security_test (id serial, content text);
INSERT INTO security_test (content) VALUES ('test document');
CREATE INDEX security_test_idx ON security_test USING bm25(content) WITH (text_config='english');

-- Create a non-superuser role
DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'test_nonsuperuser') THEN
        EXECUTE 'REASSIGN OWNED BY test_nonsuperuser TO CURRENT_USER';
        EXECUTE 'DROP OWNED BY test_nonsuperuser CASCADE';
        DROP ROLE test_nonsuperuser;
    END IF;
END $$;

CREATE ROLE test_nonsuperuser LOGIN;
GRANT ALL ON TABLE security_test TO test_nonsuperuser;
GRANT USAGE ON SCHEMA public TO test_nonsuperuser;

-- Test as superuser (should all work)
SELECT bm25_dump_index('security_test_idx') IS NOT NULL AS dump_works;
SELECT bm25_summarize_index('security_test_idx') IS NOT NULL AS summarize_works;
SELECT bm25_spill_index('security_test_idx') IS NULL AS spill_works;
SELECT bm25_force_merge('security_test_idx');

-- Switch to non-superuser
SET ROLE test_nonsuperuser;

-- Debug functions should fail with "must be superuser"
SELECT bm25_dump_index('security_test_idx');
SELECT bm25_dump_index('security_test_idx', '/tmp/should_not_exist.txt');
SELECT bm25_summarize_index('security_test_idx');
SELECT bm25_debug_pageviz('security_test_idx', '/tmp/should_not_exist.txt');

-- Admin functions should fail with "must be superuser"
SELECT bm25_spill_index('security_test_idx');
SELECT bm25_force_merge('security_test_idx');

-- REVOKE should also block non-superusers even without explicit GRANT
-- (test that REVOKE FROM PUBLIC is in effect)
SELECT has_function_privilege('test_nonsuperuser', 'bm25_spill_index(text)', 'EXECUTE') AS spill_priv;
SELECT has_function_privilege('test_nonsuperuser', 'bm25_force_merge(text)', 'EXECUTE') AS merge_priv;
SELECT has_function_privilege('test_nonsuperuser', 'bm25_dump_index(text)', 'EXECUTE') AS dump_priv;
SELECT has_function_privilege('test_nonsuperuser', 'bm25_summarize_index(text)', 'EXECUTE') AS summarize_priv;

-- Resource-control GUCs should require superuser
SET pg_textsearch.bulk_load_threshold = 1;
SET pg_textsearch.memtable_spill_threshold = 0;
SET pg_textsearch.segments_per_level = 2;

-- User-facing GUCs should still be settable
SET pg_textsearch.default_limit = 500;
SHOW pg_textsearch.default_limit;

-- Reset to superuser
RESET ROLE;

-- Cleanup
DROP TABLE security_test;
DROP OWNED BY test_nonsuperuser CASCADE;
DROP ROLE test_nonsuperuser;
DROP EXTENSION pg_textsearch;
