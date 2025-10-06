-- Test case to demonstrate DSA detach bug
-- If the bug exists, dropping one index should break other indexes
-- because tp_release_local_index_state() detaches from the shared DSA

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
SELECT COUNT(*) FROM test1 WHERE content @@ to_bm25query('hello', 'idx1');
SELECT COUNT(*) FROM test2 WHERE content @@ to_bm25query('foo', 'idx2');

-- Now drop the first index
-- This should trigger tp_relcache_callback -> tp_release_local_index_state
-- which calls dsa_detach() on the shared DSA
DROP INDEX idx1;

-- If the bug exists, this query should fail because the DSA was detached
-- Error would be something like "dsa_area could not attach to segment"
SELECT COUNT(*) FROM test2 WHERE content @@ to_bm25query('foo', 'idx2');

-- Also try creating a new index - should fail if DSA is broken
CREATE TABLE test3 (id int, content text);
INSERT INTO test3 VALUES (1, 'new test');
CREATE INDEX idx3 ON test3 USING bm25(content) WITH (text_config='english');

-- Try to use the new index
SELECT COUNT(*) FROM test3 WHERE content @@ to_bm25query('new', 'idx3');

-- Cleanup
DROP TABLE test1, test2, test3 CASCADE;
DROP EXTENSION pg_textsearch;
