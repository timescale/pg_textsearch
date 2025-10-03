-- Test case: scoring2
-- Generated BM25 test with 5 documents and 4 queries
-- Testing both bulk build and incremental build modes
CREATE EXTENSION IF NOT EXISTS pgtextsearch;
SET pgtextsearch.log_scores = true;
SET enable_seqscan = off;

-- MODE 1: Bulk build (insert data, then create index)
CREATE TABLE scoring2_bulk (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO scoring2_bulk (content) VALUES ('hello world');
INSERT INTO scoring2_bulk (content) VALUES ('goodbye world');
INSERT INTO scoring2_bulk (content) VALUES ('hello goodbye');
INSERT INTO scoring2_bulk (content) VALUES ('world domination');
INSERT INTO scoring2_bulk (content) VALUES ('hello');

-- Create index after data insertion (bulk build)
CREATE INDEX scoring2_bulk_idx ON scoring2_bulk USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Bulk mode query 1: 'hello'
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'scoring2_bulk_idx'))::numeric, 4) as score
FROM scoring2_bulk
ORDER BY content <@> to_bm25query('hello', 'scoring2_bulk_idx'), id;

-- Bulk mode query 2: 'world'
SELECT id, content, ROUND((content <@> to_bm25query('world', 'scoring2_bulk_idx'))::numeric, 4) as score
FROM scoring2_bulk
ORDER BY content <@> to_bm25query('world', 'scoring2_bulk_idx'), id;

-- Bulk mode query 3: 'goodbye'
SELECT id, content, ROUND((content <@> to_bm25query('goodbye', 'scoring2_bulk_idx'))::numeric, 4) as score
FROM scoring2_bulk
ORDER BY content <@> to_bm25query('goodbye', 'scoring2_bulk_idx'), id;

-- Bulk mode query 4: 'domination'
SELECT id, content, ROUND((content <@> to_bm25query('domination', 'scoring2_bulk_idx'))::numeric, 4) as score
FROM scoring2_bulk
ORDER BY content <@> to_bm25query('domination', 'scoring2_bulk_idx'), id;

-- MODE 2: Incremental build (create index, then insert data)
CREATE TABLE scoring2_incr (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Create index before data insertion (incremental build)
CREATE INDEX scoring2_incr_idx ON scoring2_incr USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Insert test documents incrementally
INSERT INTO scoring2_incr (content) VALUES ('hello world');
INSERT INTO scoring2_incr (content) VALUES ('goodbye world');
INSERT INTO scoring2_incr (content) VALUES ('hello goodbye');
INSERT INTO scoring2_incr (content) VALUES ('world domination');
INSERT INTO scoring2_incr (content) VALUES ('hello');

-- Incremental mode query 1: 'hello'
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'scoring2_incr_idx'))::numeric, 4) as score
FROM scoring2_incr
ORDER BY content <@> to_bm25query('hello', 'scoring2_incr_idx'), id;

-- Incremental mode query 2: 'world'
SELECT id, content, ROUND((content <@> to_bm25query('world', 'scoring2_incr_idx'))::numeric, 4) as score
FROM scoring2_incr
ORDER BY content <@> to_bm25query('world', 'scoring2_incr_idx'), id;

-- Incremental mode query 3: 'goodbye'
SELECT id, content, ROUND((content <@> to_bm25query('goodbye', 'scoring2_incr_idx'))::numeric, 4) as score
FROM scoring2_incr
ORDER BY content <@> to_bm25query('goodbye', 'scoring2_incr_idx'), id;

-- Incremental mode query 4: 'domination'
SELECT id, content, ROUND((content <@> to_bm25query('domination', 'scoring2_incr_idx'))::numeric, 4) as score
FROM scoring2_incr
ORDER BY content <@> to_bm25query('domination', 'scoring2_incr_idx'), id;

-- Cleanup
DROP TABLE scoring2_bulk CASCADE;
DROP TABLE scoring2_incr CASCADE;
DROP EXTENSION pgtextsearch CASCADE;
