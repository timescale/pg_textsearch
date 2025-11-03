-- Reproduction script for segment writing crash
-- This will create a test database, insert some data, and trigger the crash

DROP DATABASE IF EXISTS crash_test;
CREATE DATABASE crash_test;
\c crash_test

CREATE EXTENSION pg_textsearch;

CREATE TABLE test_docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO test_docs (content) VALUES
  ('hello world'),
  ('test document');

CREATE INDEX test_idx ON test_docs USING bm25 (content) WITH (text_config='english');

-- This should trigger the crash
SELECT tp_spill_memtable('test_idx');
