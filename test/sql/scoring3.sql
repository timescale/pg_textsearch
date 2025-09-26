-- Test case: scoring3
-- Generated BM25 test with 3 documents and 2 queries
-- Testing both bulk build and incremental build modes
CREATE EXTENSION IF NOT EXISTS tapir;
SET tapir.log_scores = true;
SET enable_seqscan = off;

-- MODE 1: Bulk build (insert data, then create index)
CREATE TABLE scoring3_bulk (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO scoring3_bulk (content) VALUES ('the quick brown fox jumps over the lazy dog');
INSERT INTO scoring3_bulk (content) VALUES ('a short sentence');
INSERT INTO scoring3_bulk (content) VALUES ('this is a medium length sentence that contains several words');

-- Create index after data insertion (bulk build)
CREATE INDEX scoring3_bulk_idx ON scoring3_bulk USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Bulk mode query 1: 'quick'
SELECT id, content, ROUND((content <@> to_tpquery('quick', 'scoring3_bulk_idx'))::numeric, 4) as score
FROM scoring3_bulk
ORDER BY content <@> to_tpquery('quick', 'scoring3_bulk_idx'), id;

-- Bulk mode query 2: 'sentence'
SELECT id, content, ROUND((content <@> to_tpquery('sentence', 'scoring3_bulk_idx'))::numeric, 4) as score
FROM scoring3_bulk
ORDER BY content <@> to_tpquery('sentence', 'scoring3_bulk_idx'), id;

-- MODE 2: Incremental build (create index, then insert data)
CREATE TABLE scoring3_incr (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Create index before data insertion (incremental build)
CREATE INDEX scoring3_incr_idx ON scoring3_incr USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Insert test documents incrementally
INSERT INTO scoring3_incr (content) VALUES ('the quick brown fox jumps over the lazy dog');
INSERT INTO scoring3_incr (content) VALUES ('a short sentence');
INSERT INTO scoring3_incr (content) VALUES ('this is a medium length sentence that contains several words');

-- Incremental mode query 1: 'quick'
SELECT id, content, ROUND((content <@> to_tpquery('quick', 'scoring3_incr_idx'))::numeric, 4) as score
FROM scoring3_incr
ORDER BY content <@> to_tpquery('quick', 'scoring3_incr_idx'), id;

-- Incremental mode query 2: 'sentence'
SELECT id, content, ROUND((content <@> to_tpquery('sentence', 'scoring3_incr_idx'))::numeric, 4) as score
FROM scoring3_incr
ORDER BY content <@> to_tpquery('sentence', 'scoring3_incr_idx'), id;

-- Cleanup
DROP TABLE scoring3_bulk CASCADE;
DROP TABLE scoring3_incr CASCADE;
DROP EXTENSION tapir CASCADE;
