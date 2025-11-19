-- Test case: scoring1
-- Generated BM25 test with 2 documents and 2 queries
-- Testing both bulk build and incremental build modes
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Load validation functions quietly
\i test/sql/load_validation_quiet.sql

SET pg_textsearch.log_scores = true;
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
CREATE INDEX scoring1_bulk_idx ON scoring1_bulk USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Bulk mode query 1: 'hello'
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'scoring1_bulk_idx'))::numeric, 4) as score

FROM scoring1_bulk
ORDER BY content <@> to_bm25query('hello', 'scoring1_bulk_idx'), id;

-- Validate BM25 scoring for 'hello'
SELECT validate_bm25_scoring('scoring1_bulk', 'content', 'scoring1_bulk_idx', 'hello', 'english', 1.2, 0.75) as hello_valid;

-- Bulk mode query 2: 'cruel'
SELECT id, content, ROUND((content <@> to_bm25query('cruel', 'scoring1_bulk_idx'))::numeric, 4) as score

FROM scoring1_bulk
ORDER BY content <@> to_bm25query('cruel', 'scoring1_bulk_idx'), id;

-- Validate BM25 scoring for 'cruel'
SELECT validate_bm25_scoring('scoring1_bulk', 'content', 'scoring1_bulk_idx', 'cruel', 'english', 1.2, 0.75) as cruel_valid;

-- MODE 2: Incremental build (create index, then insert data)
CREATE TABLE scoring1_incr (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Create index before data insertion (incremental build)
CREATE INDEX scoring1_incr_idx ON scoring1_incr USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Insert test documents incrementally
INSERT INTO scoring1_incr (content) VALUES ('hello world');
INSERT INTO scoring1_incr (content) VALUES ('goodbye cruel world');

-- Incremental mode query 1: 'hello'
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'scoring1_incr_idx'))::numeric, 4) as score

FROM scoring1_incr
ORDER BY content <@> to_bm25query('hello', 'scoring1_incr_idx'), id;

-- Validate BM25 scoring for 'hello' (incremental)
SELECT validate_bm25_scoring('scoring1_incr', 'content', 'scoring1_incr_idx', 'hello', 'english', 1.2, 0.75) as hello_incr_valid;

-- Incremental mode query 2: 'cruel'
SELECT id, content, ROUND((content <@> to_bm25query('cruel', 'scoring1_incr_idx'))::numeric, 4) as score

FROM scoring1_incr
ORDER BY content <@> to_bm25query('cruel', 'scoring1_incr_idx'), id;

-- Validate BM25 scoring for 'cruel' (incremental)
SELECT validate_bm25_scoring('scoring1_incr', 'content', 'scoring1_incr_idx', 'cruel', 'english', 1.2, 0.75) as cruel_incr_valid;

-- Cleanup
DROP TABLE scoring1_bulk CASCADE;
DROP TABLE scoring1_incr CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
