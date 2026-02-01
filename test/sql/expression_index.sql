-- Regression test for expression index support

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET enable_seqscan = off;

CREATE TABLE owners (
    id serial PRIMARY KEY,
    name text
);

CREATE TABLE product_logs (
    id serial PRIMARY KEY,
    data jsonb
);

INSERT INTO owners (name) VALUES
    ('Ada Lovelace'),
    ('Grace Hopper');

INSERT INTO product_logs (data) VALUES
    ('{"title": "Super Widget", "details": "High performance widget for database engineers"}'),
    ('{"title": "Mega Gadget", "details": "Optimized gadget with low latency"}');

CREATE INDEX idx_json_details ON product_logs
USING bm25 ((data->>'details'))
WITH (text_config='english', k1=1.2, b=0.75);

-- Basic ORDER BY uses implicit index resolution for the expression index.
EXPLAIN (COSTS OFF)
SELECT id
FROM product_logs
ORDER BY (data->>'details') <@> 'widget'
LIMIT 2;

SELECT id, data->>'details' AS details
FROM product_logs
ORDER BY (data->>'details') <@> 'widget', id
LIMIT 2;

-- Varno normalization: indexed table is second in the range table list.
EXPLAIN (COSTS OFF)
SELECT l.id
FROM owners o
JOIN product_logs l ON o.id = l.id
ORDER BY (l.data->>'details') <@> 'widget'
LIMIT 2;

SELECT l.id, l.data->>'details' AS details
FROM owners o
JOIN product_logs l ON o.id = l.id
ORDER BY (l.data->>'details') <@> 'widget', l.id
LIMIT 2;

-- Parallel build for expression indexes.
-- Lower thresholds so the table qualifies for parallel build.
SET min_parallel_table_scan_size = 0;
SET maintenance_work_mem = '256MB';
SET max_parallel_maintenance_workers = 2;

CREATE TABLE product_logs_parallel (
    id serial PRIMARY KEY,
    data jsonb
);

INSERT INTO product_logs_parallel (data)
SELECT jsonb_build_object(
           'title', 'Product ' || i,
           'details', 'Parallel build widget details ' || i ||
                      ' for full text search'
       )
FROM generate_series(1, 100000) AS i;

ANALYZE product_logs_parallel;

CREATE INDEX product_logs_parallel_idx ON product_logs_parallel
USING bm25 ((data->>'details'))
WITH (text_config='english', k1=1.2, b=0.75);

EXPLAIN (COSTS OFF)
SELECT id
FROM product_logs_parallel
ORDER BY (data->>'details') <@> 'widget'
LIMIT 5;

SELECT id
FROM product_logs_parallel
ORDER BY (data->>'details') <@> 'widget', id
LIMIT 5;

DROP TABLE product_logs CASCADE;
DROP TABLE owners CASCADE;
DROP TABLE product_logs_parallel CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
