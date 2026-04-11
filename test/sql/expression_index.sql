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
SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('database', 'expr_jsonb_idx'))::numeric, 4)
              AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@>
         to_bm25query('database', 'expr_jsonb_idx')
LIMIT 5;

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

SELECT id,
       ROUND(((coalesce(title, '') || ' ' || coalesce(body, '')) <@>
              to_bm25query('database', 'expr_concat_idx'))::numeric, 4)
              AS score
FROM expr_concat
ORDER BY (coalesce(title, '') || ' ' || coalesce(body, '')) <@>
         to_bm25query('database', 'expr_concat_idx')
LIMIT 5;

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

SELECT id,
       ROUND(((lower(content)) <@>
              to_bm25query('database', 'expr_lower_idx'))::numeric, 4)
              AS score
FROM expr_lower
ORDER BY (lower(content)) <@>
         to_bm25query('database', 'expr_lower_idx')
LIMIT 5;

-- ============================================================
-- Implicit resolution (no explicit index name)
-- ============================================================

SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('database'))::numeric, 4) AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@> to_bm25query('database')
LIMIT 5;

-- ============================================================
-- Verify index scan is used
-- ============================================================

EXPLAIN (COSTS OFF)
SELECT id
FROM expr_jsonb
ORDER BY (data->>'content') <@>
         to_bm25query('database', 'expr_jsonb_idx')
LIMIT 5;

-- ============================================================
-- NULL handling: row with missing JSONB key
-- ============================================================

INSERT INTO expr_jsonb (data) VALUES
    ('{"other_field": "no content key"}'),
    (NULL);

-- These should not affect results
SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('database',
                           'expr_jsonb_idx'))::numeric, 4)
              AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@>
         to_bm25query('database', 'expr_jsonb_idx')
LIMIT 5;

-- ============================================================
-- Delete a document and verify index still works
-- ============================================================

DELETE FROM expr_jsonb WHERE id = 1;

SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('search',
                           'expr_jsonb_idx'))::numeric, 4)
              AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@>
         to_bm25query('search', 'expr_jsonb_idx')
LIMIT 5;

-- ============================================================
-- VACUUM: delete rows and vacuum to exercise rebuild
-- ============================================================

DELETE FROM expr_jsonb WHERE id <= 2;
VACUUM expr_jsonb;

-- Verify index still works after vacuum
SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('search',
                           'expr_jsonb_idx'))::numeric, 4)
              AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@>
         to_bm25query('search', 'expr_jsonb_idx')
LIMIT 5;

-- ============================================================
-- Partitioned table with expression index
-- ============================================================

CREATE TABLE expr_part (
    id SERIAL,
    data JSONB,
    created_at DATE NOT NULL
) PARTITION BY RANGE (created_at);

CREATE TABLE expr_part_2024 PARTITION OF expr_part
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
CREATE TABLE expr_part_2025 PARTITION OF expr_part
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

INSERT INTO expr_part (data, created_at) VALUES
    ('{"text": "database management systems"}',
     '2024-06-15'),
    ('{"text": "machine learning algorithms"}',
     '2024-09-20'),
    ('{"text": "search engine optimization"}',
     '2025-03-10');

CREATE INDEX expr_part_idx ON expr_part
    USING bm25 ((data->>'text'))
    WITH (text_config='english');

SELECT id,
       ROUND(((data->>'text') <@>
              to_bm25query('database',
                           'expr_part_idx'))::numeric,
             4) AS score
FROM expr_part
ORDER BY (data->>'text') <@>
         to_bm25query('database', 'expr_part_idx')
LIMIT 5;

DROP TABLE expr_part CASCADE;

-- ============================================================
-- Multiple expression indexes trigger warning
-- ============================================================

CREATE INDEX expr_jsonb_idx2 ON expr_jsonb
    USING bm25 ((data->>'content'))
    WITH (text_config='simple');

-- Implicit resolution should warn about multiple indexes
SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('database'))::numeric, 4)
              AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@> to_bm25query('database')
LIMIT 3;

-- Explicit naming with multiple expression indexes exercises
-- the expression branch in collect_explicit_indexes_walker,
-- find_explicit_requirement, and fix_bm25_indexpaths.
SELECT id,
       ROUND(((data->>'content') <@>
              to_bm25query('database',
                           'expr_jsonb_idx2'))::numeric, 4)
              AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@>
         to_bm25query('database', 'expr_jsonb_idx2')
LIMIT 3;

DROP INDEX expr_jsonb_idx2;

-- ============================================================
-- Implicit text <@> text with expression index
-- ============================================================

SELECT id,
       ROUND(((data->>'content') <@> 'database')::numeric, 4)
              AS score
FROM expr_jsonb
ORDER BY (data->>'content') <@> 'database'
LIMIT 3;

-- ============================================================
-- Error cases
-- ============================================================
\set ON_ERROR_STOP off

-- Non-text expression type
CREATE INDEX expr_bad_idx ON expr_lower
    USING bm25 ((length(content)))
    WITH (text_config='simple');

-- Constant left operand with bm25query
SELECT 'hello' <@> to_bm25query('world', 'expr_jsonb_idx');

-- Constant left operand with text
SELECT 'hello' <@> 'world';

\set ON_ERROR_STOP on

-- ============================================================
-- Cleanup
-- ============================================================

DROP TABLE expr_jsonb CASCADE;
DROP TABLE expr_concat CASCADE;
DROP TABLE expr_lower CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
