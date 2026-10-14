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

RESET pg_textsearch.log_scores;

-- The extension need not be installed in public or first in search_path.
\pset format unaligned
CREATE SCHEMA bm25_real;
CREATE EXTENSION pg_textsearch SCHEMA bm25_real;
SET search_path = bm25_real, public;
CREATE TABLE schema_docs (id integer, content text, tags text[]);
INSERT INTO schema_docs VALUES
    (1, 'target', ARRAY['target']),
    (2, 'other', ARRAY['other']);
CREATE INDEX schema_text_idx ON schema_docs USING bm25(content)
    WITH (text_config='english');
CREATE INDEX schema_array_idx ON schema_docs USING bm25(tags)
    WITH (text_config='english');

CREATE SCHEMA bm25_shadow;
CREATE TYPE bm25_shadow.bm25query AS (dummy integer);
CREATE FUNCTION bm25_shadow.score(text, text) RETURNS float8
    LANGUAGE plpgsql AS $$ BEGIN RETURN 42; END $$;
CREATE FUNCTION bm25_shadow.score(text, bm25_real.bm25query) RETURNS float8
    LANGUAGE plpgsql AS $$ BEGIN RETURN 43; END $$;
CREATE FUNCTION bm25_shadow.score(text[], text) RETURNS float8
    LANGUAGE plpgsql AS $$ BEGIN RETURN 44; END $$;
CREATE FUNCTION bm25_shadow.score(text[], bm25_real.bm25query) RETURNS float8
    LANGUAGE plpgsql AS $$ BEGIN RETURN 45; END $$;
CREATE OPERATOR bm25_shadow.<@> (
    LEFTARG = text, RIGHTARG = text, FUNCTION = bm25_shadow.score);
CREATE OPERATOR bm25_shadow.<@> (
    LEFTARG = text, RIGHTARG = bm25_real.bm25query,
    FUNCTION = bm25_shadow.score);
CREATE OPERATOR bm25_shadow.<@> (
    LEFTARG = text[], RIGHTARG = text, FUNCTION = bm25_shadow.score);
CREATE OPERATOR bm25_shadow.<@> (
    LEFTARG = text[], RIGHTARG = bm25_real.bm25query,
    FUNCTION = bm25_shadow.score);
SET search_path = bm25_shadow, bm25_real, public;
-- Invalidate even an already-warm OID cache under the hostile search_path.
CREATE TYPE bm25_shadow.cache_reset AS (dummy integer);

-- Real operators must still be recognized/re-written for all four signatures.
EXPLAIN (COSTS OFF)
SELECT id FROM schema_docs
ORDER BY content OPERATOR(bm25_real.<@>) 'target'::text LIMIT 1;
SELECT id FROM schema_docs
ORDER BY content OPERATOR(bm25_real.<@>) 'target'::text LIMIT 1;
EXPLAIN (COSTS OFF)
SELECT id FROM schema_docs
ORDER BY content OPERATOR(bm25_real.<@>) 'target'::bm25_real.bm25query
LIMIT 1;
SELECT id FROM schema_docs
ORDER BY content OPERATOR(bm25_real.<@>) 'target'::bm25_real.bm25query
LIMIT 1;
EXPLAIN (COSTS OFF)
SELECT id FROM schema_docs
ORDER BY tags OPERATOR(bm25_real.<@>) 'target'::text LIMIT 1;
SELECT id FROM schema_docs
ORDER BY tags OPERATOR(bm25_real.<@>) 'target'::text LIMIT 1;
EXPLAIN (COSTS OFF)
SELECT id FROM schema_docs
ORDER BY tags OPERATOR(bm25_real.<@>) 'target'::bm25_real.bm25query LIMIT 1;
SELECT id FROM schema_docs
ORDER BY tags OPERATOR(bm25_real.<@>) 'target'::bm25_real.bm25query LIMIT 1;

-- Parser-selected shadow operators are legitimate user operators, not BM25.
-- Constant left operands must not trigger the extension's column-only check.
SELECT 'x' <@> 'target'::text AS shadow_text,
       'x' <@> 'target'::bm25_real.bm25query AS shadow_query,
       ARRAY['x'] <@> 'target'::text AS shadow_array_text,
       ARRAY['x'] <@> 'target'::bm25_real.bm25query AS shadow_array_query;
SELECT content OPERATOR(bm25_shadow.<@>) 'target'::text AS shadow_text,
       content OPERATOR(bm25_shadow.<@>)
           'target'::bm25_real.bm25query AS shadow_query,
       tags OPERATOR(bm25_shadow.<@>) 'target'::text AS shadow_array_text,
       tags OPERATOR(bm25_shadow.<@>)
           'target'::bm25_real.bm25query AS shadow_array_query
FROM schema_docs ORDER BY id;

-- Namespace renames must not leave stale schema names in the OID cache.
ALTER SCHEMA bm25_real RENAME TO bm25_renamed;
SET search_path = bm25_shadow, bm25_renamed, public;
SELECT id FROM bm25_renamed.schema_docs
ORDER BY content OPERATOR(bm25_renamed.<@>) 'target'::text LIMIT 1;
RESET search_path;
RESET enable_seqscan;
DROP INDEX bm25_renamed.schema_text_idx;
DROP INDEX bm25_renamed.schema_array_idx;
SET client_min_messages = warning;
DROP SCHEMA bm25_shadow CASCADE;
DROP EXTENSION pg_textsearch;

-- Warm the absent-extension cache, then recreate with new object OIDs.
-- This must also work on PG17 releases without an extension syscache.
SELECT 1 AS without_extension;
CREATE EXTENSION pg_textsearch SCHEMA bm25_renamed;
SET search_path = bm25_renamed, public;
CREATE INDEX schema_text_idx ON schema_docs USING bm25(content)
    WITH (text_config='english');
SET enable_seqscan = off;
SELECT id FROM schema_docs ORDER BY content <@> 'target'::text LIMIT 1;
SELECT id FROM schema_docs ORDER BY content <@> 'target'::bm25query LIMIT 1;
RESET enable_seqscan;
RESET search_path;
DROP TABLE bm25_renamed.schema_docs;
DROP EXTENSION pg_textsearch;
DROP SCHEMA bm25_renamed;
RESET client_min_messages;
\pset format aligned
