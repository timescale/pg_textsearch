-- Expression index tests for pg_textsearch

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- ============================================================
-- JSONB expression index
-- ============================================================

CREATE TABLE expr_jsonb (
    id SERIAL PRIMARY KEY,
    data JSONB
);

INSERT INTO expr_jsonb (data) VALUES
    ('{"content": "postgresql database management system"}'),
    ('{"content": "machine learning algorithms"}'),
    ('{"content": "full text search and retrieval"}');

CREATE INDEX expr_jsonb_idx ON expr_jsonb
    USING bm25 ((data->>'content'))
    WITH (text_config='english');

SET enable_seqscan = off;

-- Query with explicit index naming
-- Expression indexes require the expression in the query to match
-- the index expression exactly.  The scan path does not yet resolve
-- expression-based <@> operands, so this errors for now.
\set ON_ERROR_STOP off

SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('database', 'expr_jsonb_idx'))::numeric, 4)
              AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@>
         to_bm25query('database', 'expr_jsonb_idx')
LIMIT 5;

\set ON_ERROR_STOP on

-- ============================================================
-- Multi-column concatenation expression index
-- ============================================================

CREATE TABLE expr_concat (
    id SERIAL PRIMARY KEY,
    title TEXT,
    body TEXT
);

INSERT INTO expr_concat (title, body) VALUES
    ('Database Systems', 'An introduction to relational databases'),
    ('Machine Learning', 'Algorithms for pattern recognition'),
    ('Search Engines', 'Full text search and information retrieval');

CREATE INDEX expr_concat_idx ON expr_concat
    USING bm25 ((coalesce(title, '') || ' ' || coalesce(body, '')))
    WITH (text_config='english');

\set ON_ERROR_STOP off

SELECT id,
       ROUND(((coalesce(title, '') || ' ' || coalesce(body, '')) <@>
              to_bm25query('database', 'expr_concat_idx'))::numeric, 4)
              AS score
FROM expr_concat
ORDER BY (coalesce(title, '') || ' ' || coalesce(body, '')) <@>
         to_bm25query('database', 'expr_concat_idx')
LIMIT 5;

\set ON_ERROR_STOP on

-- ============================================================
-- Simple transformation expression index
-- ============================================================

CREATE TABLE expr_lower (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO expr_lower (content) VALUES
    ('PostgreSQL Database Management'),
    ('Machine Learning Algorithms'),
    ('Full Text Search and Retrieval');

CREATE INDEX expr_lower_idx ON expr_lower
    USING bm25 ((lower(content)))
    WITH (text_config='simple');

\set ON_ERROR_STOP off

SELECT id,
       ROUND(((lower(content)) <@>
              to_bm25query('database', 'expr_lower_idx'))::numeric, 4)
              AS score
FROM expr_lower
ORDER BY (lower(content)) <@>
         to_bm25query('database', 'expr_lower_idx')
LIMIT 5;

\set ON_ERROR_STOP on

-- ============================================================
-- Error case: non-text expression type
-- ============================================================
\set ON_ERROR_STOP off

CREATE INDEX expr_bad_idx ON expr_lower
    USING bm25 ((length(content)))
    WITH (text_config='simple');

\set ON_ERROR_STOP on

-- ============================================================
-- Cleanup
-- ============================================================

DROP TABLE expr_jsonb CASCADE;
DROP TABLE expr_concat CASCADE;
DROP TABLE expr_lower CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
