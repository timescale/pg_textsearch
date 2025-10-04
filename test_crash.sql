DROP TABLE IF EXISTS test_docs CASCADE;
DROP EXTENSION IF EXISTS pgtextsearch CASCADE;

CREATE EXTENSION pgtextsearch;
CREATE TABLE test_docs (id int, content text);
INSERT INTO test_docs VALUES (1, 'hello world'), (2, 'test document');
CREATE INDEX test_idx ON test_docs USING bm25(content) WITH (text_config='english');

-- This should crash
SELECT to_bm25vector('test', 'test_idx');
