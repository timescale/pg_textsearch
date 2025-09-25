-- Test case: goodbye_debug
-- Generated BM25 test with 3 documents and 1 queries
CREATE EXTENSION IF NOT EXISTS tapir;
SET tapir.log_scores = true;
SET enable_seqscan = off;

-- Create test table
CREATE TABLE goodbye_debug_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO goodbye_debug_docs (content) VALUES ('hello world');
INSERT INTO goodbye_debug_docs (content) VALUES ('goodbye cruel world');
INSERT INTO goodbye_debug_docs (content) VALUES ('goodbye nerds');

-- Create Tapir index
CREATE INDEX goodbye_debug_idx ON goodbye_debug_docs USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Test query 1: 'goodbye'
SELECT id, content, ROUND((content <@> to_tpquery('goodbye', 'goodbye_debug_idx'))::numeric, 4) as score
FROM goodbye_debug_docs
ORDER BY content <@> to_tpquery('goodbye', 'goodbye_debug_idx'), id;

-- Cleanup
DROP TABLE goodbye_debug_docs CASCADE;
DROP EXTENSION tapir CASCADE;
