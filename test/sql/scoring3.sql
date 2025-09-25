-- Test case: document_length_normalization
-- Generated BM25 test with 3 documents and 4 queries
CREATE EXTENSION IF NOT EXISTS tapir;
SET tapir.log_scores = true;
SET enable_seqscan = off;

-- Create test table
CREATE TABLE document_length_normalization_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO document_length_normalization_docs (content) VALUES ('short doc');
INSERT INTO document_length_normalization_docs (content) VALUES ('this is a medium length document with more words');
INSERT INTO document_length_normalization_docs (content) VALUES ('this is a very long document that contains many words and should demonstrate how BM25 normalizes scores based on document length to avoid bias toward shorter documents');

-- Create Tapir index
CREATE INDEX document_length_normalization_idx ON document_length_normalization_docs USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Test query 1: 'document'
SELECT id, content, ROUND((content <@> to_tpquery('document', 'document_length_normalization_idx'))::numeric, 4) as score
FROM document_length_normalization_docs
ORDER BY content <@> to_tpquery('document', 'document_length_normalization_idx'), id;

-- Test query 2: 'length'
SELECT id, content, ROUND((content <@> to_tpquery('length', 'document_length_normalization_idx'))::numeric, 4) as score
FROM document_length_normalization_docs
ORDER BY content <@> to_tpquery('length', 'document_length_normalization_idx'), id;

-- Test query 3: 'short'
SELECT id, content, ROUND((content <@> to_tpquery('short', 'document_length_normalization_idx'))::numeric, 4) as score
FROM document_length_normalization_docs
ORDER BY content <@> to_tpquery('short', 'document_length_normalization_idx'), id;

-- Test query 4: 'words'
SELECT id, content, ROUND((content <@> to_tpquery('words', 'document_length_normalization_idx'))::numeric, 4) as score
FROM document_length_normalization_docs
ORDER BY content <@> to_tpquery('words', 'document_length_normalization_idx'), id;

-- Cleanup
DROP TABLE document_length_normalization_docs CASCADE;
DROP EXTENSION tapir CASCADE;
