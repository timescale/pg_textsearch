-- Test binary I/O for bm25query type (COPY BINARY)

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create a table and index for testing
CREATE TABLE binary_io_docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO binary_io_docs (content) VALUES
    ('hello world test'),
    ('database search query');

CREATE INDEX binary_io_idx ON binary_io_docs USING bm25(content)
    WITH (text_config='english');

-- Create a table with bm25query column
CREATE TABLE query_export (id SERIAL, q bm25query);

-- Insert some queries
INSERT INTO query_export (q) VALUES
    (to_bm25query('hello world', 'binary_io_idx')),
    (to_bm25query('database search', 'binary_io_idx'));

-- Show original data before COPY
SELECT id, q::text AS original_query FROM query_export ORDER BY id;

-- Test COPY TO BINARY (exercises tpquery_send)
COPY query_export TO '/tmp/query_export.bin' WITH (FORMAT binary);

-- Test COPY FROM BINARY (exercises tpquery_recv)
CREATE TABLE query_import (id int, q bm25query);
COPY query_import FROM '/tmp/query_export.bin' WITH (FORMAT binary);

-- Verify data was imported correctly
SELECT COUNT(*) AS imported_count FROM query_import;

-- Verify the queries are equivalent
SELECT
    e.id,
    e.q::text AS exported,
    i.q::text AS imported,
    e.q = i.q AS queries_match
FROM query_export e
JOIN query_import i ON e.id = i.id
ORDER BY e.id;

-- =============================================================================
-- Test bm25vector binary I/O (exercises vector.c recv/send functions)
-- =============================================================================

-- Create table with bm25vector column for binary copy test
CREATE TABLE vector_export (id serial, v bm25vector);
INSERT INTO vector_export (v) VALUES
    ('binary_io_idx:{hello:1,world:2}'::bm25vector),
    ('binary_io_idx:{database:3,search:1}'::bm25vector);

-- Export via COPY BINARY (exercises bm25vector_send)
COPY vector_export TO '/tmp/vector_export.bin' WITH (FORMAT binary);

-- Import via COPY BINARY (exercises bm25vector_recv)
CREATE TABLE vector_import (id int, v bm25vector);
COPY vector_import FROM '/tmp/vector_export.bin' WITH (FORMAT binary);

-- Verify round-trip
SELECT COUNT(*) AS vector_imported_count FROM vector_import;

-- Clean up
\! rm -f /tmp/query_export.bin /tmp/vector_export.bin
DROP TABLE query_export;
DROP TABLE query_import;
DROP TABLE vector_export;
DROP TABLE vector_import;
DROP TABLE binary_io_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
