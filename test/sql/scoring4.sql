-- Test case: scoring4
-- Generated BM25 test with 2 documents and 1 queries
-- Testing both bulk build and incremental build modes
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET enable_seqscan = off;

-- MODE 1: Bulk build (insert data, then create index)
CREATE TABLE scoring4_bulk (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO scoring4_bulk (content) VALUES ('goodbye world');
INSERT INTO scoring4_bulk (content) VALUES ('goodbyes are hard');

-- Create index after data insertion (bulk build)
CREATE INDEX scoring4_bulk_idx ON scoring4_bulk USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Bulk mode query 1: 'goodbye'
SELECT id, content, ROUND((content <@> to_bm25query('goodbye', 'scoring4_bulk_idx'))::numeric, 4) as score
FROM scoring4_bulk
ORDER BY content <@> to_bm25query('goodbye', 'scoring4_bulk_idx'), id;

-- MODE 2: Incremental build (create index, then insert data)
CREATE TABLE scoring4_incr (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Create index before data insertion (incremental build)
CREATE INDEX scoring4_incr_idx ON scoring4_incr USING bm25(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Insert test documents incrementally
INSERT INTO scoring4_incr (content) VALUES ('goodbye world');
INSERT INTO scoring4_incr (content) VALUES ('goodbyes are hard');

-- Incremental mode query 1: 'goodbye'
SELECT id, content, ROUND((content <@> to_bm25query('goodbye', 'scoring4_incr_idx'))::numeric, 4) as score
FROM scoring4_incr
ORDER BY content <@> to_bm25query('goodbye', 'scoring4_incr_idx'), id;

-- Cleanup
DROP TABLE scoring4_bulk CASCADE;
DROP TABLE scoring4_incr CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
