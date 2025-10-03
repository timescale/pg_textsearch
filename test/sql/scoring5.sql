-- Test case: scoring5
-- Generated BM25 test with 3 documents and 4 queries
-- Testing both bulk build and incremental build modes
CREATE EXTENSION IF NOT EXISTS pgtextsearch;
SET pgtextsearch.log_scores = true;
SET enable_seqscan = off;

-- MODE 1: Bulk build (insert data, then create index)
CREATE TABLE scoring5_bulk (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO scoring5_bulk (content) VALUES ('hello world');
INSERT INTO scoring5_bulk (content) VALUES ('goodbye cruel world');
INSERT INTO scoring5_bulk (content) VALUES ('goodbye nerds');

-- Create index after data insertion (bulk build)
CREATE INDEX scoring5_bulk_idx ON scoring5_bulk USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Bulk mode query 1: 'hello'
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'scoring5_bulk_idx'))::numeric, 4) as score
FROM scoring5_bulk
ORDER BY content <@> to_bm25query('hello', 'scoring5_bulk_idx'), id;

-- Bulk mode query 2: 'cruel'
SELECT id, content, ROUND((content <@> to_bm25query('cruel', 'scoring5_bulk_idx'))::numeric, 4) as score
FROM scoring5_bulk
ORDER BY content <@> to_bm25query('cruel', 'scoring5_bulk_idx'), id;

-- Bulk mode query 3: 'world'
SELECT id, content, ROUND((content <@> to_bm25query('world', 'scoring5_bulk_idx'))::numeric, 4) as score
FROM scoring5_bulk
ORDER BY content <@> to_bm25query('world', 'scoring5_bulk_idx'), id;

-- Bulk mode query 4: 'goodbye'
SELECT id, content, ROUND((content <@> to_bm25query('goodbye', 'scoring5_bulk_idx'))::numeric, 4) as score
FROM scoring5_bulk
ORDER BY content <@> to_bm25query('goodbye', 'scoring5_bulk_idx'), id;

-- MODE 2: Incremental build (create index, then insert data)
CREATE TABLE scoring5_incr (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Create index before data insertion (incremental build)
CREATE INDEX scoring5_incr_idx ON scoring5_incr USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Insert test documents incrementally
INSERT INTO scoring5_incr (content) VALUES ('hello world');
INSERT INTO scoring5_incr (content) VALUES ('goodbye cruel world');
INSERT INTO scoring5_incr (content) VALUES ('goodbye nerds');

-- Incremental mode query 1: 'hello'
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'scoring5_incr_idx'))::numeric, 4) as score
FROM scoring5_incr
ORDER BY content <@> to_bm25query('hello', 'scoring5_incr_idx'), id;

-- Incremental mode query 2: 'cruel'
SELECT id, content, ROUND((content <@> to_bm25query('cruel', 'scoring5_incr_idx'))::numeric, 4) as score
FROM scoring5_incr
ORDER BY content <@> to_bm25query('cruel', 'scoring5_incr_idx'), id;

-- Incremental mode query 3: 'world'
SELECT id, content, ROUND((content <@> to_bm25query('world', 'scoring5_incr_idx'))::numeric, 4) as score
FROM scoring5_incr
ORDER BY content <@> to_bm25query('world', 'scoring5_incr_idx'), id;

-- Incremental mode query 4: 'goodbye'
SELECT id, content, ROUND((content <@> to_bm25query('goodbye', 'scoring5_incr_idx'))::numeric, 4) as score
FROM scoring5_incr
ORDER BY content <@> to_bm25query('goodbye', 'scoring5_incr_idx'), id;

-- Cleanup
DROP TABLE scoring5_bulk CASCADE;
DROP TABLE scoring5_incr CASCADE;
DROP EXTENSION pgtextsearch CASCADE;
