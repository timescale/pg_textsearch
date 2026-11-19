CREATE INDEX documents_bm25 ON documents
USING bm25 (body) WITH (text_config = 'english', k1 = 1.2, b = 0.75);
VACUUM (ANALYZE) documents;
