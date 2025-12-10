-- Test case: schema
-- Tests index operations with schema-qualified tables
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET pg_textsearch.log_scores = true;
SET enable_seqscan = off;

-- Create a custom schema
CREATE SCHEMA docs;

-- Create table in the custom schema
CREATE TABLE docs.articles (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO docs.articles (content) VALUES ('hello world');
INSERT INTO docs.articles (content) VALUES ('goodbye cruel world');
INSERT INTO docs.articles (content) VALUES ('hello cruel goodbye');

-- Create index on schema-qualified table
CREATE INDEX articles_idx ON docs.articles USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Test 1: Unqualified index name (not in search path - should fail)
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'articles_idx'))::numeric, 4) as score
FROM docs.articles
ORDER BY content <@> to_bm25query('hello', 'articles_idx'), id;

-- Test 2: Schema-qualified index name works correctly
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'docs.articles_idx'))::numeric, 4) as score
FROM docs.articles
ORDER BY content <@> to_bm25query('hello', 'docs.articles_idx'), id;

-- Test 3: With search_path set, unqualified names work
SET search_path = docs, public;
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'articles_idx'))::numeric, 4) as score
FROM articles
ORDER BY content <@> to_bm25query('hello', 'articles_idx'), id;

-- Reset search_path
SET search_path = public;

-- Test 4: Table in public schema works with unqualified names
CREATE TABLE public_articles (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO public_articles (content) VALUES ('hello world');
INSERT INTO public_articles (content) VALUES ('goodbye cruel world');

CREATE INDEX public_articles_idx ON public_articles USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Unqualified index name works for public schema tables
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'public_articles_idx'))::numeric, 4) as score
FROM public_articles
ORDER BY content <@> to_bm25query('hello', 'public_articles_idx'), id;

-- Test 5: INSERT operations with schema-qualified tables and implicit index
-- names (reproduces the reported bug)
CREATE SCHEMA bar;
CREATE TABLE bar.baz(content TEXT NOT NULL);

-- Create index with implicit name (PostgreSQL will name it baz_content_idx)
CREATE INDEX ON bar.baz USING bm25(content)
  WITH (text_config='english');

-- INSERTs work correctly with schema-qualified tables
INSERT INTO bar.baz VALUES('really');
INSERT INTO bar.baz VALUES('testing schemas');
INSERT INTO bar.baz VALUES('another document');

-- Verify data was inserted correctly
SELECT * FROM bar.baz ORDER BY content;

-- Test search functionality
SELECT content, ROUND((content <@> to_bm25query('testing',
  'bar.baz_content_idx'))::numeric, 4) as score
FROM bar.baz
ORDER BY content <@> to_bm25query('testing', 'bar.baz_content_idx'), content;

-- Test BM25 edge case: With only 2 documents where a term appears in exactly
-- 50% of them (1 out of 2), the IDF becomes log(1) = 0, making the entire
-- BM25 score 0 regardless of term frequency. This is mathematically correct
-- behavior, not a bug.
CREATE SCHEMA edge_case;
CREATE TABLE edge_case.demo(content TEXT NOT NULL);
CREATE INDEX ON edge_case.demo USING bm25(content) WITH (text_config='english');

-- Insert exactly 2 documents, one with "testing"
INSERT INTO edge_case.demo VALUES('really');
INSERT INTO edge_case.demo VALUES('testing schemas');

-- With 2 docs and "testing" in 1 (50%), IDF = log((2-1+0.5)/(1+0.5)) = log(1) = 0
SELECT content, ROUND((content <@> to_bm25query('testing',
  'edge_case.demo_content_idx'))::numeric, 4) as score
FROM edge_case.demo
ORDER BY content <@> to_bm25query('testing', 'edge_case.demo_content_idx'), content;

DROP TABLE edge_case.demo CASCADE;
DROP SCHEMA edge_case CASCADE;

-- Test 6: bm25_dump_index with schema-qualified indexes
-- Test that the debug function properly handles schema resolution

-- Test debug with public schema index (should work)
SELECT bm25_dump_index('public_articles_idx')::text ~ 'Tapir Index Debug: public_articles_idx' AS debug_public_works;

-- Test debug with schema-qualified index name (should work)
SELECT bm25_dump_index('bar.baz_content_idx')::text ~ 'Tapir Index Debug: bar.baz_content_idx' AS debug_schema_qualified;

-- Test debug with unqualified name for schema index (should fail when not in search path)
SELECT bm25_dump_index('baz_content_idx')::text ~ 'ERROR: Index ''baz_content_idx'' not found' AS debug_unqualified_fails;

-- Test debug with search_path set
SET search_path = bar, public;
SELECT bm25_dump_index('baz_content_idx')::text ~ 'Tapir Index Debug: baz_content_idx' AS debug_with_search_path;

-- Reset search_path
SET search_path = public;

-- Test debug with non-existent index
SELECT bm25_dump_index('nonexistent_idx')::text ~ 'ERROR: Index ''nonexistent_idx'' not found' AS debug_nonexistent;

-- Cleanup
DROP TABLE bar.baz CASCADE;
DROP SCHEMA bar CASCADE;
DROP TABLE docs.articles CASCADE;
DROP TABLE public_articles CASCADE;
DROP SCHEMA docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
