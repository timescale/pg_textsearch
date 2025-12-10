-- Test handling of empty and whitespace-only documents

-- Ensure extension is loaded
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test table with various empty/whitespace content
CREATE TABLE empty_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert various empty/whitespace documents
INSERT INTO empty_docs (content) VALUES
    (''),                           -- completely empty
    ('   '),                        -- only spaces
    ('		'),                      -- only tabs
    ('

    '),                             -- only newlines
    ('
  	 '),                         -- mixed whitespace
    (NULL);                         -- NULL content

-- Create index on content
CREATE INDEX empty_docs_idx ON empty_docs
USING bm25 (content)
WITH (text_config = 'english');

-- Try searching - should return no results without warnings
-- Note: WHERE with score comparison requires explicit index reference
SELECT id, content
FROM empty_docs
WHERE content <@> to_bm25query('test', 'empty_docs_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'empty_docs_idx')
LIMIT 5;

-- Add a document with actual content
INSERT INTO empty_docs (content) VALUES ('test document with content');

-- Search again - should find only the document with content
SELECT id, substring(content, 1, 30) as content_preview
FROM empty_docs
WHERE content <@> to_bm25query('test', 'empty_docs_idx') < -0.001
ORDER BY content <@> to_bm25query('test', 'empty_docs_idx')
LIMIT 5;

-- Clean up
DROP TABLE empty_docs;
DROP EXTENSION pg_textsearch;
