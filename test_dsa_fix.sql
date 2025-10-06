-- Test case to verify DSA detach bug is fixed
-- Tests that dropping one index doesn't break other indexes

\set VERBOSITY verbose
\set ECHO all

-- Start fresh
DROP EXTENSION IF EXISTS pg_textsearch CASCADE;
DROP TABLE IF EXISTS test1, test2, test3 CASCADE;

-- Create extension
CREATE EXTENSION pg_textsearch;

-- Create two tables with indexes
CREATE TABLE test1 (id int, content text);
CREATE TABLE test2 (id int, content text);

INSERT INTO test1 VALUES (1, 'hello world'), (2, 'goodbye world');
INSERT INTO test2 VALUES (1, 'foo bar'), (2, 'baz qux');

-- Create two indexes
CREATE INDEX idx1 ON test1 USING bm25(content) WITH (text_config='english');
CREATE INDEX idx2 ON test2 USING bm25(content) WITH (text_config='english');

-- Both indexes should work
SELECT COUNT(*) FROM test1 WHERE content <@> to_bm25query('hello', 'idx1') < 0;
SELECT COUNT(*) FROM test2 WHERE content <@> to_bm25query('foo', 'idx2') < 0;

-- Now drop the first index
-- This used to call dsa_detach() and break the shared DSA
DROP INDEX idx1;

-- Test that idx2 still works (bug would cause "dsa_area could not attach" error)
SELECT COUNT(*) FROM test2 WHERE content <@> to_bm25query('foo', 'idx2') < 0;

-- Also try creating a new index - should work if DSA is healthy
CREATE TABLE test3 (id int, content text);
INSERT INTO test3 VALUES (1, 'new test');
CREATE INDEX idx3 ON test3 USING bm25(content) WITH (text_config='english');

-- Try to use the new index
SELECT COUNT(*) FROM test3 WHERE content <@> to_bm25query('new', 'idx3') < 0;

-- All tests passed!
\echo 'TEST PASSED: DSA detach bug is fixed!'

-- Cleanup
DROP TABLE test1, test2, test3 CASCADE;
DROP EXTENSION pg_textsearch;
