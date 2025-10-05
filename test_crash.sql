-- Simple test to reproduce crash
CREATE EXTENSION IF NOT EXISTS pgtextsearch;
CREATE TABLE test_doc (id serial PRIMARY KEY, content text);
INSERT INTO test_doc (content) VALUES ('test document');
CREATE INDEX test_idx ON test_doc USING bm25(content) WITH (text_config='english');

-- This query pattern causes the crash
SELECT content <@> to_bm25query('test', 'test_idx') as score
FROM test_doc
ORDER BY content <@> to_bm25query('test', 'test_idx') ASC
LIMIT 5;

DROP TABLE test_doc CASCADE;
