-- Test cases for queries that are NOT YET fully supported
-- These document known limitations of the current implicit index resolution
-- See DESIGN_INDEX_ERGONOMICS.md for details on PlaceHolderVar and future work

-- Load extension
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
-- LIMITATION 1: JOINs with score in SELECT
-- The CTID cache may not preserve scores correctly through JOIN operations
-- because the score is computed during the scan but projected after the JOIN.
-- =============================================================================

-- This query joins docs with categories and tries to return the score.
-- The score cache uses CTID, which may not map correctly after JOIN processing.
\echo 'Test: JOIN with score in SELECT list'
EXPLAIN (COSTS OFF)
SELECT d.id, d.content, c.category, d.content <@> 'database' as score
FROM docs d
JOIN categories c ON d.id = c.doc_id
ORDER BY d.content <@> 'database'
LIMIT 5;

-- Execute to verify behavior (may show incorrect scores or zeros)
SELECT d.id, d.content, c.category,
       ROUND((d.content <@> 'database')::numeric, 4) as score
FROM docs d
JOIN categories c ON d.id = c.doc_id
ORDER BY d.content <@> 'database'
LIMIT 5;

-- =============================================================================
-- LIMITATION 2: Subqueries with score referenced in outer query
-- When the score is computed in a subquery but referenced outside,
-- the CTID cache cannot preserve it across the subquery boundary.
-- =============================================================================

\echo 'Test: Subquery with score referenced outside'
EXPLAIN (COSTS OFF)
SELECT * FROM (
    SELECT id, content, content <@> 'machine' as score
    FROM docs
    ORDER BY content <@> 'machine'
    LIMIT 10
) sub
WHERE score < 0;

-- Execute to see actual behavior
SELECT * FROM (
    SELECT id, content,
           ROUND((content <@> 'machine')::numeric, 4) as score
    FROM docs
    ORDER BY content <@> 'machine'
    LIMIT 10
) sub
WHERE score < 0;

-- =============================================================================
-- LIMITATION 3: CTEs (Common Table Expressions) with score
-- Similar to subqueries - score computed in CTE may not be preserved
-- when referenced in the main query.
-- =============================================================================

\echo 'Test: CTE with score'
EXPLAIN (COSTS OFF)
WITH ranked_docs AS (
    SELECT id, content, content <@> 'search' as score
    FROM docs
    ORDER BY content <@> 'search'
    LIMIT 10
)
SELECT * FROM ranked_docs WHERE score < -0.5;

-- Execute to see actual behavior
WITH ranked_docs AS (
    SELECT id, content,
           ROUND((content <@> 'search')::numeric, 4) as score
    FROM docs
    ORDER BY content <@> 'search'
    LIMIT 10
)
SELECT * FROM ranked_docs WHERE score < -0.5;

-- =============================================================================
-- LIMITATION 4: Multiple BM25 indexes on the same column
-- When there are multiple BM25 indexes on the same column (e.g., with
-- different text_config), the planner hook may pick the wrong one.
-- =============================================================================

\echo 'Test: Multiple BM25 indexes on same column'

-- Create a second index with different config
CREATE INDEX docs_bm25_simple_idx ON docs USING bm25(content)
    WITH (text_config='simple');

-- The planner hook finds the first matching index, which may not be
-- the one the user intended. Use explicit to_bm25query() for control.
EXPLAIN (COSTS OFF)
SELECT id, content <@> 'database' as score
FROM docs
ORDER BY content <@> 'database'
LIMIT 3;

-- With explicit index, user has control:
EXPLAIN (COSTS OFF)
SELECT id, content <@> to_bm25query('database', 'docs_bm25_simple_idx') as score
FROM docs
ORDER BY content <@> to_bm25query('database', 'docs_bm25_simple_idx')
LIMIT 3;

DROP INDEX docs_bm25_simple_idx;

-- =============================================================================
-- LIMITATION 5: Expression indexes
-- KNOWN BUG: Expression indexes (e.g., lower(content)) cause server crash
-- during index build. DO NOT test until this is fixed.
-- =============================================================================

\echo 'Test: Expression index - SKIPPED (causes server crash)'

-- The following would crash the server:
-- CREATE INDEX docs_lower_idx ON docs USING bm25(lower(content))
--     WITH (text_config='simple');
--
-- This is a known limitation - expression indexes are not currently supported.
-- Attempting to create one will crash PostgreSQL.

-- =============================================================================
-- LIMITATION 6: Cross-table JOINs with BM25 on multiple tables
-- When both tables have BM25 indexes and both are used in the query,
-- each must resolve to its own index correctly.
-- =============================================================================

\echo 'Test: Cross-table JOINs with multiple BM25 indexes'

CREATE TABLE other_docs (
    id SERIAL PRIMARY KEY,
    body TEXT
);

INSERT INTO other_docs (body) VALUES
    ('database optimization techniques'),
    ('search engine algorithms');

CREATE INDEX other_docs_idx ON other_docs USING bm25(body)
    WITH (text_config='english');

-- Query that scores both tables - each should resolve to correct index
EXPLAIN (COSTS OFF)
SELECT d.id, o.id,
       d.content <@> 'database' as doc_score,
       o.body <@> 'database' as other_score
FROM docs d, other_docs o
ORDER BY d.content <@> 'database'
LIMIT 5;

-- Execute to verify both scores work
SELECT d.id as doc_id, o.id as other_id,
       ROUND((d.content <@> 'database')::numeric, 4) as doc_score,
       ROUND((o.body <@> 'database')::numeric, 4) as other_score
FROM docs d, other_docs o
ORDER BY d.content <@> 'database'
LIMIT 5;

DROP TABLE other_docs CASCADE;

-- =============================================================================
-- LIMITATION 7: UNION queries
-- BUG: When UNION uses index-ordered scans from multiple tables, scores are
-- computed correctly for the index scan but then the CTID cache returns
-- stale scores for subsequent rows. This causes all rows to show the same
-- score as the first result.
-- =============================================================================

\echo 'Test: UNION with scores'

CREATE TABLE docs2 (id serial, content text);
INSERT INTO docs2 (content) VALUES ('another database document');
CREATE INDEX docs2_idx ON docs2 USING bm25(content) WITH (text_config='english');

-- Scores from different indexes may not be directly comparable
EXPLAIN (COSTS OFF)
SELECT id, content, content <@> 'database' as score, 'docs' as source
FROM docs
UNION ALL
SELECT id, content, content <@> 'database' as score, 'docs2' as source
FROM docs2
ORDER BY score
LIMIT 5;

-- Execute to see actual behavior
-- NOTE: All rows show -0.9808 even though only docs 1 and docs2.1 contain "database"
-- This is a known bug - the CTID cache returns stale scores
SELECT id, content,
       ROUND((content <@> 'database')::numeric, 4) as score,
       'docs' as source
FROM docs
UNION ALL
SELECT id, content,
       ROUND((content <@> 'database')::numeric, 4) as score,
       'docs2' as source
FROM docs2
ORDER BY score
LIMIT 5;

DROP TABLE docs2 CASCADE;

-- =============================================================================
-- LIMITATION 8: Aggregate functions on scores
-- BUG: The WHERE clause with text <@> text uses the CTID cache, which may
-- contain stale scores from previous queries. This causes incorrect filtering.
-- In this example, all 3 rows pass the WHERE clause even though only 1
-- contains "database".
-- =============================================================================

\echo 'Test: Aggregate on scores'

-- NOTE: Returns 3 matching docs even though only doc 1 contains "database"
-- This is due to CTID cache returning stale scores from previous query
SELECT COUNT(*) as matching_docs,
       ROUND(AVG((content <@> 'database')::numeric), 4) as avg_score,
       ROUND(MIN((content <@> 'database')::numeric), 4) as best_score
FROM docs
WHERE content <@> 'database' < 0;

-- =============================================================================
-- LIMITATION 9: Window functions with scores
-- Window functions may evaluate score multiple times or at wrong time
-- =============================================================================

\echo 'Test: Window functions with scores'

EXPLAIN (COSTS OFF)
SELECT id, content,
       content <@> 'database' as score,
       ROW_NUMBER() OVER (ORDER BY content <@> 'database') as rank
FROM docs
ORDER BY content <@> 'database'
LIMIT 5;

-- Execute to verify behavior
SELECT id, content,
       ROUND((content <@> 'database')::numeric, 4) as score,
       ROW_NUMBER() OVER (ORDER BY content <@> 'database') as rank
FROM docs
ORDER BY content <@> 'database'
LIMIT 5;

-- =============================================================================
-- WORKAROUND EXAMPLES
-- These show how to handle unsupported cases using explicit syntax
-- =============================================================================

\echo 'Workaround: Use explicit to_bm25query for complex queries'

-- For JOINs, compute score explicitly with index name
SELECT d.id, d.content, c.category,
       ROUND((d.content <@> to_bm25query('database', 'docs_bm25_idx'))::numeric, 4) as score
FROM docs d
JOIN categories c ON d.id = c.doc_id
ORDER BY d.content <@> to_bm25query('database', 'docs_bm25_idx')
LIMIT 5;

-- Cleanup
DROP TABLE categories CASCADE;
DROP TABLE docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
