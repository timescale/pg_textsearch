DROP DATABASE IF EXISTS simple_test;
CREATE DATABASE simple_test;
\c simple_test;
CREATE EXTENSION tapir;
CREATE TABLE docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO docs (content) VALUES ('hello world test document');
CREATE INDEX test_idx ON docs USING tapir(content) WITH (text_config='english');
SELECT id, content FROM docs ORDER BY content <@> to_tpquery('hello', 'test_idx') LIMIT 1;
