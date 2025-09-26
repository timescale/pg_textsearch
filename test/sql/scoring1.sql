-- Test case: scoring1
-- Generated BM25 test with 2 documents and 2 queries
-- Testing both bulk build and incremental build modes
CREATE EXTENSION IF NOT EXISTS tapir;
SET tapir.log_scores = true;
SET enable_seqscan = off;

-- MODE 1: Bulk build (insert data, then create index)
CREATE TABLE scoring1_bulk (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO scoring1_bulk (content) VALUES ('hello world');
INSERT INTO scoring1_bulk (content) VALUES ('goodbye cruel world');

-- Create index after data insertion (bulk build)
CREATE INDEX scoring1_bulk_idx ON scoring1_bulk USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Bulk mode query 1: 'hello'
SELECT id, content, ROUND((content <@> to_tpquery('hello', 'scoring1_bulk_idx'))::numeric, 4) as score
FROM scoring1_bulk
ORDER BY content <@> to_tpquery('hello', 'scoring1_bulk_idx'), id;

-- Bulk mode query 2: 'cruel'
SELECT id, content, ROUND((content <@> to_tpquery('cruel', 'scoring1_bulk_idx'))::numeric, 4) as score
FROM scoring1_bulk
ORDER BY content <@> to_tpquery('cruel', 'scoring1_bulk_idx'), id;

-- MODE 2: Incremental build (create index, then insert data)
CREATE TABLE scoring1_incr (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Create index before data insertion (incremental build)
CREATE INDEX scoring1_incr_idx ON scoring1_incr USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Insert test documents incrementally
INSERT INTO scoring1_incr (content) VALUES ('hello world');
INSERT INTO scoring1_incr (content) VALUES ('goodbye cruel world');

-- Incremental mode query 1: 'hello'
SELECT id, content, ROUND((content <@> to_tpquery('hello', 'scoring1_incr_idx'))::numeric, 4) as score
FROM scoring1_incr
ORDER BY content <@> to_tpquery('hello', 'scoring1_incr_idx'), id;

-- Incremental mode query 2: 'cruel'
SELECT id, content, ROUND((content <@> to_tpquery('cruel', 'scoring1_incr_idx'))::numeric, 4) as score
FROM scoring1_incr
ORDER BY content <@> to_tpquery('cruel', 'scoring1_incr_idx'), id;

-- Cleanup
DROP TABLE scoring1_bulk CASCADE;
DROP TABLE scoring1_incr CASCADE;
DROP EXTENSION tapir CASCADE;
