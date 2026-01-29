-- Test security restrictions on debug functions

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
GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA public TO test_nonsuperuser;

-- Test as superuser (should all work)
SELECT bm25_dump_index('security_test_idx') IS NOT NULL AS dump_works;
SELECT bm25_summarize_index('security_test_idx') IS NOT NULL AS summarize_works;

-- Switch to non-superuser
SET ROLE test_nonsuperuser;

-- These should all fail with "must be superuser"
SELECT bm25_dump_index('security_test_idx');
SELECT bm25_dump_index('security_test_idx', '/tmp/should_not_exist.txt');
SELECT bm25_summarize_index('security_test_idx');
SELECT bm25_spill_index('security_test_idx');
SELECT bm25_debug_pageviz('security_test_idx', '/tmp/should_not_exist.txt');

-- Reset to superuser
RESET ROLE;

-- Cleanup
DROP TABLE security_test;
DROP OWNED BY test_nonsuperuser CASCADE;
DROP ROLE test_nonsuperuser;
