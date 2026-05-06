-- Test indexing of documents whose unique-token volume would otherwise
-- exceed Postgres's tsvector 1 MB lexeme dictionary cap.
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET client_min_messages = WARNING;

-- Sanity: bare to_tsvector still rejects oversized inputs.
DO $$
DECLARE
    big_doc text;
BEGIN
    SELECT string_agg('w' || gs::text, ' ')
      INTO big_doc
      FROM generate_series(1, 250000) gs;
    PERFORM to_tsvector('english', big_doc);
    RAISE EXCEPTION 'to_tsvector unexpectedly succeeded';
EXCEPTION WHEN program_limit_exceeded THEN
    RAISE NOTICE 'to_tsvector rejected oversized input as expected';
END $$;

-- CREATE INDEX over a >2 MB many-unique-token document must succeed.
CREATE TABLE large_docs (id int, content text);
INSERT INTO large_docs VALUES (1, 'short doc with hello world');
INSERT INTO large_docs (id, content)
SELECT 2, string_agg('w' || gs::text, ' ')
  FROM generate_series(1, 250000) gs;
INSERT INTO large_docs VALUES (3, 'another short doc mentioning hello');

SELECT id, length(content) > 1048576 AS oversized FROM large_docs ORDER BY id;

CREATE INDEX large_idx ON large_docs USING bm25 (content text_bm25_ops)
    WITH (text_config = 'english');

-- Index scan: a token from the large doc must be retrievable.
SELECT id FROM large_docs
ORDER BY content <@> to_bm25query('w12345', 'large_idx')
LIMIT 1;

-- Index scan over a token that is in two short docs (sanity).
SELECT id FROM large_docs
ORDER BY content <@> to_bm25query('hello', 'large_idx')
LIMIT 5;

-- INSERT (aminsert) of a >2 MB many-unique-token row.
INSERT INTO large_docs (id, content)
SELECT 4, string_agg('z' || gs::text, ' ')
  FROM generate_series(1, 250000) gs;

SELECT id FROM large_docs
ORDER BY content <@> to_bm25query('z67890', 'large_idx')
LIMIT 1;

-- VACUUM after deleting the large rows must succeed.
DELETE FROM large_docs WHERE id IN (2, 4);
VACUUM large_docs;

DROP TABLE large_docs;
DROP EXTENSION pg_textsearch CASCADE;
