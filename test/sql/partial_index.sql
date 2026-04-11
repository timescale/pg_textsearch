-- Partial index tests for pg_textsearch

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- ============================================================
-- Basic partial index
-- ============================================================

CREATE TABLE partial_docs (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category TEXT
);

INSERT INTO partial_docs (content, category) VALUES
    ('postgresql database management', 'tech'),
    ('machine learning algorithms', 'tech'),
    ('cooking recipes for beginners', 'lifestyle'),
    ('full text search and retrieval', 'tech'),
    ('gardening tips and tricks', 'lifestyle');

CREATE INDEX partial_tech_idx ON partial_docs
    USING bm25 (content)
    WITH (text_config='english')
    WHERE category = 'tech';

SET enable_seqscan = off;

-- Explicit query against partial index
SELECT id, content,
       ROUND((content <@>
              to_bm25query('database',
                           'partial_tech_idx'))::numeric, 4)
              AS score
FROM partial_docs
WHERE category = 'tech'
ORDER BY content <@>
         to_bm25query('database', 'partial_tech_idx')
LIMIT 5;

-- Lifestyle rows should not appear
SELECT id, content,
       ROUND((content <@>
              to_bm25query('cooking',
                           'partial_tech_idx'))::numeric, 4)
              AS score
FROM partial_docs
WHERE category = 'tech'
ORDER BY content <@>
         to_bm25query('cooking', 'partial_tech_idx')
LIMIT 5;

-- ============================================================
-- Multi-language use case
-- ============================================================

CREATE TABLE multilang (
    id SERIAL PRIMARY KEY,
    content TEXT,
    language TEXT
);

INSERT INTO multilang (content, language) VALUES
    ('the quick brown fox jumps over the lazy dog', 'english'),
    ('postgresql database management system', 'english'),
    ('full text searching and information retrieval', 'english'),
    ('simple words without stemming needed', 'simple'),
    ('another simple document for testing', 'simple');

CREATE INDEX multilang_english_idx ON multilang
    USING bm25 (content)
    WITH (text_config='english')
    WHERE language = 'english';

CREATE INDEX multilang_simple_idx ON multilang
    USING bm25 (content)
    WITH (text_config='simple')
    WHERE language = 'simple';

-- English stemming: "databases" matches "database"
SELECT id, content,
       ROUND((content <@>
              to_bm25query('databases',
                           'multilang_english_idx'))::numeric,
             4) AS score
FROM multilang
WHERE language = 'english'
ORDER BY content <@>
         to_bm25query('databases', 'multilang_english_idx')
LIMIT 5;

-- Simple: no stemming
SELECT id, content,
       ROUND((content <@>
              to_bm25query('simple',
                           'multilang_simple_idx'))::numeric,
             4) AS score
FROM multilang
WHERE language = 'simple'
ORDER BY content <@>
         to_bm25query('simple', 'multilang_simple_idx')
LIMIT 5;

-- ============================================================
-- Expression + partial combined
-- ============================================================

CREATE TABLE expr_partial (
    id SERIAL PRIMARY KEY,
    data JSONB
);

INSERT INTO expr_partial (data) VALUES
    ('{"text": "postgresql database management", "type": "article"}'),
    ('{"text": "machine learning algorithms", "type": "article"}'),
    ('{"text": "random user comment", "type": "comment"}'),
    ('{"text": "full text search retrieval", "type": "article"}');

CREATE INDEX expr_partial_idx ON expr_partial
    USING bm25 ((data->>'text'))
    WITH (text_config='english')
    WHERE (data->>'type') = 'article';

SELECT id,
       ROUND(((data->>'text') <@>
              to_bm25query('database',
                           'expr_partial_idx'))::numeric,
             4) AS score
FROM expr_partial
WHERE (data->>'type') = 'article'
ORDER BY (data->>'text') <@>
         to_bm25query('database', 'expr_partial_idx')
LIMIT 5;

-- ============================================================
-- Insert into partial index -- predicate filtering
-- ============================================================

-- Insert a tech row that matches the predicate and query
INSERT INTO partial_docs (content, category) VALUES
    ('advanced database optimization techniques', 'tech');

-- New tech row should appear in results
SELECT id, content,
       ROUND((content <@>
              to_bm25query('database',
                           'partial_tech_idx'))::numeric, 4)
              AS score
FROM partial_docs
WHERE category = 'tech'
ORDER BY content <@>
         to_bm25query('database', 'partial_tech_idx')
LIMIT 5;

-- Insert a row that does NOT match the predicate
INSERT INTO partial_docs (content, category) VALUES
    ('database administration basics', 'lifestyle');

-- It should not appear in partial index results
SELECT id, content,
       ROUND((content <@>
              to_bm25query('database',
                           'partial_tech_idx'))::numeric, 4)
              AS score
FROM partial_docs
WHERE category = 'tech'
ORDER BY content <@>
         to_bm25query('database', 'partial_tech_idx')
LIMIT 5;

-- ============================================================
-- Verify partial index scan is used
-- ============================================================

EXPLAIN (COSTS OFF)
SELECT id
FROM partial_docs
WHERE category = 'tech'
ORDER BY content <@>
         to_bm25query('database', 'partial_tech_idx')
LIMIT 5;

-- ============================================================
-- Cleanup
-- ============================================================

DROP TABLE partial_docs CASCADE;
DROP TABLE multilang CASCADE;
DROP TABLE expr_partial CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
