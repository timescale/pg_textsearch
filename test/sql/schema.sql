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

-- Cleanup
DROP TABLE docs.articles CASCADE;
DROP TABLE public_articles CASCADE;
DROP SCHEMA docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
