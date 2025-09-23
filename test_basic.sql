-- Basic functionality test
CREATE EXTENSION tapir;

-- Test basic table and index creation
CREATE TABLE test_docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO test_docs (content) VALUES ('hello world test');

-- Create Tapir index
CREATE INDEX test_idx ON test_docs USING tapir(content) WITH (text_config = 'english');

-- Simple test query
SELECT * FROM test_docs WHERE content <@> to_tpquery('test_idx:{hello}');

-- Cleanup
DROP INDEX test_idx;
DROP TABLE test_docs;
DROP EXTENSION tapir;