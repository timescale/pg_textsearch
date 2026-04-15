-- Test BM25 index on text[] columns
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Basic text array table and index
CREATE TABLE docs_array (
    id SERIAL PRIMARY KEY,
    content text[]
);

CREATE INDEX docs_array_bm25_idx ON docs_array
    USING bm25(content) WITH (text_config='english');

INSERT INTO docs_array (content) VALUES
    (ARRAY['the quick brown fox', 'jumped over the lazy dog']),
    (ARRAY['database system design', 'query optimization']),
    (ARRAY['full text search', 'inverted index structure']),
    (ARRAY['postgres extensions', 'custom access methods']),
    (ARRAY['information retrieval', 'relevance ranking']);

-- BM25 search using explicit bm25query
SELECT id, content
FROM docs_array
ORDER BY content <@> to_bm25query('database', 'docs_array_bm25_idx')
LIMIT 3;

-- Score equivalence: text[] scores should match scalar concatenation
-- Create equivalent scalar table
CREATE TABLE docs_scalar (
    id SERIAL PRIMARY KEY,
    content text
);

CREATE INDEX docs_scalar_bm25_idx ON docs_scalar
    USING bm25(content) WITH (text_config='english');

INSERT INTO docs_scalar (content) VALUES
    ('the quick brown fox jumped over the lazy dog'),
    ('database system design query optimization'),
    ('full text search inverted index structure'),
    ('postgres extensions custom access methods'),
    ('information retrieval relevance ranking');

-- Scores from text[] and text should match for same content
SELECT
    a.id,
    a.content <@> to_bm25query('database',
        'docs_array_bm25_idx') AS array_score,
    s.content <@> to_bm25query('database',
        'docs_scalar_bm25_idx') AS scalar_score
FROM docs_array a
JOIN docs_scalar s ON a.id = s.id
WHERE a.id = 2
ORDER BY a.id;

-- NULL handling: NULL elements should be skipped
INSERT INTO docs_array (content) VALUES
    (ARRAY['hello world', NULL, 'test document']);

SELECT id, content
FROM docs_array
ORDER BY content <@> to_bm25query('hello', 'docs_array_bm25_idx')
LIMIT 1;

-- NULL array should be skipped (no crash)
INSERT INTO docs_array (content) VALUES (NULL);

-- Empty array should be handled gracefully
INSERT INTO docs_array (content) VALUES (ARRAY[]::text[]);

-- Search still works after NULLs and empties
SELECT id, content
FROM docs_array
ORDER BY content <@> to_bm25query('database', 'docs_array_bm25_idx')
LIMIT 1;

-- UPDATE a text[] row and verify search picks up changes
UPDATE docs_array SET content = ARRAY['updated database', 'new content']
WHERE id = 1;

SELECT id, content
FROM docs_array
ORDER BY content <@> to_bm25query('updated', 'docs_array_bm25_idx')
LIMIT 1;

-- DELETE and verify
DELETE FROM docs_array WHERE id = 2;

SELECT count(*) FROM docs_array
WHERE id = 2;

-- Implicit text[] <@> text resolution
SELECT id, content
FROM docs_array
ORDER BY content <@> 'postgres'
LIMIT 1;

-- Clean up
DROP TABLE docs_array CASCADE;
DROP TABLE docs_scalar CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
