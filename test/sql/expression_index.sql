-- Regression test for expression index support

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET enable_seqscan = off;

CREATE TABLE product_logs (
    id serial PRIMARY KEY,
    data jsonb
);

INSERT INTO product_logs (data) VALUES
    ('{"title": "Super Widget", "details": "High performance widget for database engineers"}'),
    ('{"title": "Mega Gadget", "details": "Optimized gadget with low latency"}');

CREATE INDEX idx_json_details ON product_logs
USING bm25 ((data->>'details'))
WITH (text_config='english', k1=1.2, b=0.75);

EXPLAIN (COSTS OFF)
SELECT id
FROM product_logs
ORDER BY (data->>'details') <@> to_bm25query('widget')
LIMIT 2;

SELECT id, data->>'details' AS details
FROM product_logs
ORDER BY (data->>'details') <@> 'widget', id
LIMIT 2;

DROP TABLE product_logs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
