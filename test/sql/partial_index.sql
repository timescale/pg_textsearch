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
    WITH (text_config='english', compaction='manual')
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
    lang CHAR(2) NOT NULL
);

INSERT INTO multilang (content, lang) VALUES
    ('the quick brown fox jumps over the lazy dog', 'en'),
    ('postgresql database management system', 'en'),
    ('full text searching and information retrieval', 'en'),
    ('simple words without stemming needed', 'xx'),
    ('another simple document for testing', 'xx');

CREATE INDEX multilang_en_idx ON multilang
    USING bm25 (content)
    WITH (text_config='english')
    WHERE lang = 'en';

CREATE INDEX multilang_xx_idx ON multilang
    USING bm25 (content)
    WITH (text_config='simple')
    WHERE lang = 'xx';

-- English stemming: "databases" matches "database"
SELECT id, content,
       ROUND((content <@>
              to_bm25query('databases',
                           'multilang_en_idx'))::numeric,
             4) AS score
FROM multilang
WHERE lang = 'en'
ORDER BY content <@>
         to_bm25query('databases', 'multilang_en_idx')
LIMIT 5;

-- Simple: no stemming
SELECT id, content,
       ROUND((content <@>
              to_bm25query('simple',
                           'multilang_xx_idx'))::numeric,
             4) AS score
FROM multilang
WHERE lang = 'xx'
ORDER BY content <@>
         to_bm25query('simple', 'multilang_xx_idx')
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
-- VACUUM on partial index
-- ============================================================

DELETE FROM partial_docs WHERE id = 1;
VACUUM partial_docs;

-- Verify partial index still works after vacuum
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
-- Implicit resolution skips partial indexes
-- ============================================================

-- Table with ONLY a partial index (no non-partial BM25 index)
CREATE TABLE partial_only (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category TEXT
);

INSERT INTO partial_only (content, category) VALUES
    ('database management', 'tech'),
    ('cooking recipes', 'lifestyle');

CREATE INDEX partial_only_idx ON partial_only
    USING bm25 (content)
    WITH (text_config='english')
    WHERE category = 'tech';

-- Implicit resolution should ERROR because only partial
-- indexes exist on this column.
\set ON_ERROR_STOP off
SELECT id,
       ROUND((content <@> 'database')::numeric, 4) AS score
FROM partial_only
ORDER BY content <@> 'database'
LIMIT 5;
\set ON_ERROR_STOP on

DROP TABLE partial_only CASCADE;

-- ============================================================
-- Cost: a partial index covering a small share of the heap
-- ============================================================

-- The ordered scan returns at most the index's 10 documents,
-- so it must not be priced as fetching all 10000 heap tuples.
RESET enable_seqscan;

CREATE TABLE partial_cost (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category TEXT
);

INSERT INTO partial_cost (content, category)
SELECT 'database document ' || i,
       CASE WHEN i % 1000 = 0 THEN 'tech' ELSE 'other' END
FROM generate_series(1, 10000) i;

CREATE INDEX partial_cost_idx ON partial_cost
    USING bm25 (content)
    WITH (text_config='english')
    WHERE category = 'tech';

ANALYZE partial_cost;

EXPLAIN (COSTS OFF)
SELECT id
FROM partial_cost
WHERE category = 'tech'
ORDER BY content <@> to_bm25query('database', 'partial_cost_idx')
LIMIT 20;

DROP TABLE partial_cost CASCADE;

-- ============================================================
-- Cleanup
-- ============================================================

DROP TABLE partial_docs CASCADE;
DROP TABLE multilang CASCADE;
DROP TABLE expr_partial CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
