-- Test implicit index resolution via planner hook
-- Tests that to_bm25query() without an index name automatically finds the BM25 index

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test table with BM25 index
CREATE TABLE implicit_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE INDEX implicit_docs_idx ON implicit_docs USING bm25(content)
    WITH (text_config='english');

-- Insert test documents
INSERT INTO implicit_docs (content) VALUES
    ('hello world example'),
    ('database system design'),
    ('hello database world');

-- Test 1: Explicit index name works (baseline)
SELECT id, content, ROUND((content <@> to_bm25query('hello', 'implicit_docs_idx'))::numeric, 4) as score
FROM implicit_docs
WHERE content <@> to_bm25query('hello', 'implicit_docs_idx') < 0
ORDER BY score, id;

-- Test 2: Implicit index resolution (main feature being tested)
SELECT id, content, ROUND((content <@> to_bm25query('hello'))::numeric, 4) as score
FROM implicit_docs
WHERE content <@> to_bm25query('hello') < 0
ORDER BY score, id;

-- Test 3: Implicit resolution works with different query terms
SELECT id, content, ROUND((content <@> to_bm25query('database'))::numeric, 4) as score
FROM implicit_docs
WHERE content <@> to_bm25query('database') < 0
ORDER BY score, id;

-- Test 4: Works with ORDER BY only (no WHERE clause)
SELECT id, content, ROUND((content <@> to_bm25query('database'))::numeric, 4) as score
FROM implicit_docs
ORDER BY content <@> to_bm25query('database'), id
LIMIT 3;

-- Test 5: Multiple BM25 indexes on different columns - should pick correct one
CREATE TABLE multi_col_docs (
    id SERIAL PRIMARY KEY,
    title TEXT,
    body TEXT
);

CREATE INDEX multi_col_title_idx ON multi_col_docs USING bm25(title)
    WITH (text_config='english');
CREATE INDEX multi_col_body_idx ON multi_col_docs USING bm25(body)
    WITH (text_config='english');

INSERT INTO multi_col_docs (title, body) VALUES
    ('hello title', 'goodbye body content'),
    ('goodbye title', 'hello body content'),
    ('neutral title', 'neutral body content');

-- Query on title column should use title index
SELECT id, title, ROUND((title <@> to_bm25query('hello'))::numeric, 4) as score
FROM multi_col_docs
WHERE title <@> to_bm25query('hello') < 0
ORDER BY score, id;

-- Query on body column should use body index
SELECT id, body, ROUND((body <@> to_bm25query('hello'))::numeric, 4) as score
FROM multi_col_docs
WHERE body <@> to_bm25query('hello') < 0
ORDER BY score, id;

-- Test 6: Schema-qualified table still works with implicit resolution
CREATE SCHEMA implicit_schema;
CREATE TABLE implicit_schema.schema_docs (
    id SERIAL PRIMARY KEY,
    content TEXT
);

CREATE INDEX schema_docs_idx ON implicit_schema.schema_docs USING bm25(content)
    WITH (text_config='english');

INSERT INTO implicit_schema.schema_docs (content) VALUES
    ('hello from schema'),
    ('goodbye from schema');

-- Implicit resolution should work even with schema-qualified tables
SELECT id, content, ROUND((content <@> to_bm25query('hello'))::numeric, 4) as score
FROM implicit_schema.schema_docs
WHERE content <@> to_bm25query('hello') < 0
ORDER BY score, id;

-- Test 7: text <@> text implicit syntax for standalone scoring
-- The planner hook transforms text <@> text to text <@> bm25query for non-ORDER BY
-- expressions, enabling implicit index resolution without to_bm25query().
SELECT id, content, ROUND((content <@> 'hello')::numeric, 4) as score
FROM implicit_docs
WHERE content <@> to_bm25query('hello') < 0
ORDER BY score, id;

-- Test 8: text <@> text works in ORDER BY with implicit index
SELECT id, content
FROM implicit_docs
ORDER BY content <@> 'database'
LIMIT 3;

-- Test 9: Combining implicit text <@> text scoring with ORDER BY
SELECT id, content, ROUND((content <@> 'hello')::numeric, 4) as score
FROM implicit_docs
ORDER BY content <@> 'hello', id
LIMIT 3;

-- Test 10: Prepared statements require explicit index name
-- Implicit resolution doesn't work because to_bm25query($1) can't be
-- folded to a constant at plan time when the parameter isn't known.
PREPARE explicit_search(text) AS
SELECT id, content, ROUND((content <@> to_bm25query($1, 'implicit_docs_idx'))::numeric, 4) as score
FROM implicit_docs
WHERE content <@> to_bm25query($1, 'implicit_docs_idx') < 0
ORDER BY score, id;

EXECUTE explicit_search('hello');
EXECUTE explicit_search('database');

DEALLOCATE explicit_search;

-- Cleanup
DROP TABLE implicit_schema.schema_docs CASCADE;
DROP SCHEMA implicit_schema CASCADE;
DROP TABLE multi_col_docs CASCADE;
DROP TABLE implicit_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
