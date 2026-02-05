-- Regression test for expression index support

CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET enable_seqscan = off;
    

-- ============================================================
-- Basic expression index test: json->>'field'
-- ============================================================
CREATE TABLE expr_idx_product_logs (
    id serial PRIMARY KEY,
    data jsonb
);

INSERT INTO expr_idx_product_logs (data) VALUES
    ('{"title": "Super Widget", "details": "High performance widget for database engineers"}'),
    ('{"title": "Mega Gadget", "details": "Optimized gadget with low latency"}'),
    -- test it still works when 'details' is NULL
    ('{"title": "Untitled"}');

CREATE INDEX expr_idx_idx_json_details ON expr_idx_product_logs
USING bm25 ((data->>'details'))
WITH (text_config='english', k1=1.2, b=0.75);

-- Check if index is used by the planner
EXPLAIN (COSTS OFF)
SELECT id, data->>'details' AS details
FROM expr_idx_product_logs
ORDER BY (data->>'details') <@> 'widget', id
LIMIT 2;

-- Index resolution: expr <@> 'query'
SELECT id, data->>'details' AS details
FROM expr_idx_product_logs
ORDER BY (data->>'details') <@> 'widget', id
LIMIT 2;

-- Index resolution: expr <@> bm25query('query')
SELECT id, data->>'details' AS details
FROM expr_idx_product_logs
ORDER BY (data->>'details') <@> bm25query('widget'), id
LIMIT 2;

-- Explicit index
SELECT id, data->>'details' AS details
FROM expr_idx_product_logs
ORDER BY (data->>'details') <@> to_bm25query('widget', 'expr_idx_idx_json_details'), id
LIMIT 2;


-- ============================================================
-- expr that references 2 columns (2 Var nodes)
-- ============================================================
CREATE TABLE expr_idx_news (
    title text,
    content text
);

INSERT INTO expr_idx_news VALUES
    ('Postgres search extension', 'pg_textsearch 0.5.0 released'),
    ('Postgres is eating the database world', 'a solid core with many amazing extensions'),
    (NULL, 'foo bar buzz'),
    ('foo bar buzz woo', NULL);

CREATE INDEX expr_idx_idx_news ON expr_idx_news
USING bm25 ((coalesce(title, '') || ' ' || coalesce(content, '')))
WITH (text_config='english', k1=1.2, b=0.75);

EXPLAIN (COSTS OFF)
SELECT title
FROM expr_idx_news
ORDER BY ((coalesce(title, '') || ' ' || coalesce(content, ''))) <@> 'foo', title
LIMIT 3;

SELECT title
FROM expr_idx_news
ORDER BY ((coalesce(title, '') || ' ' || coalesce(content, ''))) <@> 'foo', title
LIMIT 3;

SELECT title
FROM expr_idx_news
ORDER BY ((coalesce(title, '') || ' ' || coalesce(content, ''))) <@> bm25query('foo'), title
LIMIT 3;

SELECT title
FROM expr_idx_news
ORDER BY ((coalesce(title, '') || ' ' || coalesce(content, ''))) <@> to_bm25query('foo', 'expr_idx_idx_news'), title
LIMIT 3;


-- =====================================================================
-- Varno normalization: indexed table is second in the range table list.
-- =====================================================================
CREATE TABLE expr_idx_owners (
    id serial PRIMARY KEY,
    name text
);

INSERT INTO expr_idx_owners(name) VALUES
    ('Ada Lovelace'),
    ('Grace Hopper');

EXPLAIN (COSTS OFF)
SELECT l.id
FROM expr_idx_owners o JOIN expr_idx_product_logs l 
ON o.id = l.id
ORDER BY (l.data->>'details') <@> 'widget', l.id
LIMIT 2;

SELECT l.id
FROM expr_idx_owners o JOIN expr_idx_product_logs l 
ON o.id = l.id
ORDER BY (l.data->>'details') <@> 'widget', l.id
LIMIT 2;

SELECT l.id
FROM expr_idx_owners o JOIN expr_idx_product_logs l 
ON o.id = l.id
ORDER BY (l.data->>'details') <@> bm25query('widget'), l.id
LIMIT 2;

SELECT l.id
FROM expr_idx_owners o JOIN expr_idx_product_logs l 
ON o.id = l.id
ORDER BY (l.data->>'details') <@> to_bm25query('widget', 'expr_idx_idx_json_details'), l.id
LIMIT 2;


-- =====================================================================
-- Parallel build for expression indexes.
-- Lower thresholds so the table qualifies for parallel build.
-- =====================================================================
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = '256MB';
SET max_parallel_maintenance_workers = 2;

CREATE TABLE expr_idx_product_logs_parallel (
    id serial PRIMARY KEY,
    data jsonb
);

INSERT INTO expr_idx_product_logs_parallel (data)
SELECT jsonb_build_object(
           'title', 'Product ' || i,
           'details', 'Parallel build widget details ' || i ||
                      ' for full text search'
       )
FROM generate_series(1, 100000) AS i;

-- Collect heap tuple count
ANALYZE expr_idx_product_logs_parallel;

CREATE INDEX expr_idx_product_logs_parallel_idx ON expr_idx_product_logs_parallel
USING bm25 ((data->>'details'))
WITH (text_config='english', k1=1.2, b=0.75);

EXPLAIN (COSTS OFF)
SELECT id
FROM expr_idx_product_logs_parallel
ORDER BY (data->>'details') <@> 'widget', id
LIMIT 5;

SELECT id
FROM expr_idx_product_logs_parallel
ORDER BY (data->>'details') <@> 'widget', id
LIMIT 5;

SELECT id
FROM expr_idx_product_logs_parallel
ORDER BY (data->>'details') <@> bm25query('widget'), id
LIMIT 5;

SELECT id
FROM expr_idx_product_logs_parallel
ORDER BY (data->>'details') <@> to_bm25query('widget', 'expr_idx_product_logs_parallel_idx'), id
LIMIT 5;


-- =====================================================================
-- Expression result must be text; non-text expressions should error.
-- =====================================================================
\set ON_ERROR_STOP off
CREATE INDEX wont_exist ON expr_idx_product_logs
USING bm25 ((length(data->>'details')))
WITH (text_config='english', k1=1.2, b=0.75);
\set ON_ERROR_STOP on


-- =====================================================================
-- If expr references 2 relations, index resolution should be skipped
-- And the query should fail at runtime
-- =====================================================================
\set ON_ERROR_STOP off
SELECT l.id
FROM expr_idx_owners o JOIN expr_idx_product_logs l 
ON o.id = l.id
ORDER BY (l.data->>'details' || o.id::text) <@> 'query'
LIMIT 2;

SELECT l.id
FROM expr_idx_owners o JOIN expr_idx_product_logs l 
ON o.id = l.id
ORDER BY (l.data->>'details' || o.id::text) <@> bm25query('query')
LIMIT 2;
\set ON_ERROR_STOP on


-- =====================================================================
-- If expr does not reference any relation, index resolution should be
-- skipped, and the query should fail at runtime
-- =====================================================================
\set ON_ERROR_STOP off
SELECT l.id
FROM expr_idx_product_logs l
ORDER BY ('constant') <@> 'query'
LIMIT 2;

SELECT l.id
FROM expr_idx_product_logs l
ORDER BY ('constant') <@> bm25query('query')
LIMIT 2;
\set ON_ERROR_STOP on


-- =====================================================================
-- If expr contains a correlated subquery, index resolution should be
-- skipped, and the query should fail at runtime
-- =====================================================================
\set ON_ERROR_STOP off
SELECT l.id
FROM expr_idx_product_logs l
ORDER BY (SELECT l.data->>'details') <@> 'widget'
LIMIT 2;

SELECT l.id
FROM expr_idx_product_logs l
ORDER BY (SELECT l.data->>'details') <@> bm25query('widget')
LIMIT 2;
\set ON_ERROR_STOP on


-- =====================================================================
-- Cleanup
-- =====================================================================
DROP TABLE expr_idx_product_logs CASCADE;
DROP TABLE expr_idx_news CASCADE;
DROP TABLE expr_idx_owners CASCADE;
DROP TABLE expr_idx_product_logs_parallel CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
