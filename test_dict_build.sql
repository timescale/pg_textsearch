-- Simple test script to verify dictionary building doesn't crash
-- Run with: psql -d postgres -f test_dict_build.sql

-- Create a fresh test database
DROP DATABASE IF EXISTS test_segments;
CREATE DATABASE test_segments;
\c test_segments

-- Create extension
CREATE EXTENSION pg_textsearch;

-- Create test table and data
CREATE TABLE docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO docs (content) VALUES
  ('hello world this is a test'),
  ('another document with different words'),
  ('hello again from the test system'),
  ('the quick brown fox jumps over the lazy dog'),
  ('dictionary builder test document');

-- Create index with text_config
CREATE INDEX docs_idx ON docs USING bm25 (content) WITH (text_config='english');

-- Test dictionary building
SELECT tp_spill_memtable('docs_idx');

-- If we got here, dictionary building didn't crash!
SELECT 'Dictionary building test passed!' AS result;
