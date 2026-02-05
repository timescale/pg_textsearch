-- Test cases for queries that are NOT YET fully supported
-- These document known limitations of the current implicit index resolution

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Setup test tables
CREATE TABLE docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE TABLE categories (
    id SERIAL PRIMARY KEY,
    doc_id INT REFERENCES docs(id),
    category TEXT
);

INSERT INTO docs (content) VALUES
    ('postgresql database management'),
    ('machine learning algorithms'),
    ('text search and retrieval');

INSERT INTO categories (doc_id, category) VALUES
    (1, 'technology'),
    (2, 'science'),
    (3, 'technology');

CREATE INDEX docs_bm25_idx ON docs USING bm25(content) WITH (text_config='english');

SET enable_seqscan = off;

-- =============================================================================
-- LIMITATION 1: JOINs with implicit scoring don't use the index
-- The query works but falls back to seq scan + standalone scoring.
-- This means it returns all documents (non-matching get score 0) vs explicit
-- to_bm25query() which uses the index and only returns matching documents.
-- =============================================================================

\echo 'Test: JOIN with implicit score - works but returns all docs (no index)'
SELECT d.id, d.content, c.category,
       ROUND((d.content <@> 'database')::numeric, 4) as score
FROM docs d
JOIN categories c ON d.id = c.doc_id
ORDER BY d.content <@> 'database'
LIMIT 5;

-- The workaround is to use explicit to_bm25query() for score projection
\echo 'Test: JOIN with explicit to_bm25query - works correctly'
SELECT d.id, d.content, c.category,
       ROUND((d.content <@> to_bm25query('database', 'docs_bm25_idx'))::numeric, 4) as score
FROM docs d
JOIN categories c ON d.id = c.doc_id
ORDER BY d.content <@> to_bm25query('database', 'docs_bm25_idx')
LIMIT 5;

-- =============================================================================
-- LIMITATION 2: Multiple BM25 indexes on same column
-- The planner hook picks the first matching index. Use explicit to_bm25query()
-- for control over which index is used.
-- =============================================================================

\echo 'Test: Multiple BM25 indexes on same column'

CREATE INDEX docs_bm25_simple_idx ON docs USING bm25(content)
    WITH (text_config='simple');

-- Implicit resolution picks first index found
-- Note: Score projection requires explicit index, so just use ORDER BY
EXPLAIN (COSTS OFF)
SELECT id
FROM docs
ORDER BY content <@> 'database'
LIMIT 3;

-- Explicit index selection gives user control
EXPLAIN (COSTS OFF)
SELECT id, content <@> to_bm25query('database', 'docs_bm25_simple_idx') as score
FROM docs
ORDER BY content <@> to_bm25query('database', 'docs_bm25_simple_idx')
LIMIT 3;

DROP INDEX docs_bm25_simple_idx;

-- =============================================================================
-- LIMITATION 3: Aggregate functions on scores require explicit to_bm25query()
-- =============================================================================

\echo 'Test: Aggregate on scores with explicit index'

SELECT COUNT(*) as matching_docs,
       ROUND(AVG((content <@> to_bm25query('database', 'docs_bm25_idx'))::numeric), 4) as avg_score
FROM docs;

-- =============================================================================
-- LIMITATION 4: text <@> text operator doesn't use index scans
-- The planner hook transforms text <@> text to text <@> bm25query, but the
-- transformed expression doesn't match for index ordered scans.
-- Workaround: Use to_bm25query() function (with or without index name).
-- =============================================================================

\echo 'Test: text <@> text operator vs to_bm25query()'

-- text <@> text: planner transforms but doesn't use index scan
EXPLAIN (COSTS OFF)
SELECT id, content <@> 'database' as score
FROM docs
ORDER BY content <@> 'database'
LIMIT 3;

-- to_bm25query(): properly uses index scan
EXPLAIN (COSTS OFF)
SELECT id, content <@> to_bm25query('database') as score
FROM docs
ORDER BY content <@> to_bm25query('database')
LIMIT 3;

-- Cleanup
DROP TABLE categories CASCADE;
DROP TABLE docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
