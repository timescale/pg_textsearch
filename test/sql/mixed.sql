-- Test concurrent operations on Tapir indexes
-- This test verifies that concurrent access to shared memory structures is safe
-- and that operations like inserts, searches, and index building work correctly

CREATE EXTENSION IF NOT EXISTS tapir;

-- Clean up from any previous tests
DROP TABLE IF EXISTS concurrent_test_docs CASCADE;
DROP TABLE IF EXISTS concurrent_test_docs2 CASCADE;

-- Create test tables for concurrent operations
CREATE TABLE concurrent_test_docs (
    id SERIAL PRIMARY KEY,
    content TEXT NOT NULL,
    category VARCHAR(50),
    created_at TIMESTAMP DEFAULT NOW()
);

CREATE TABLE concurrent_test_docs2 (
    id SERIAL PRIMARY KEY,
    content TEXT NOT NULL,
    priority INTEGER DEFAULT 1
);

-- Test 1: Sequential inserts to verify basic functionality
\echo 'Test 1: Sequential baseline test'

INSERT INTO concurrent_test_docs (content, category) VALUES
('database query optimization techniques', 'tech'),
('concurrent programming patterns', 'tech'),
('shared memory management systems', 'tech'),
('lock-free data structures', 'tech'),
('database indexing strategies', 'tech');

-- Create index after initial data
CREATE INDEX concurrent_idx1 ON concurrent_test_docs USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

-- Verify basic search works
SELECT id, content, ROUND((content <@> to_tpvector('database concurrent', 'concurrent_idx1'))::numeric, 4) as score
FROM concurrent_test_docs 
ORDER BY score DESC;

-- Test 2: Simulate concurrent inserts with same terms
\echo 'Test 2: Concurrent inserts with overlapping terms'

-- Insert documents that will compete for same posting lists
INSERT INTO concurrent_test_docs (content, category) VALUES
('database systems require concurrent access control', 'tech'),
('concurrent database operations need careful synchronization', 'tech'),
('database concurrent processing improves performance', 'tech');

-- Search to verify posting lists updated correctly
SELECT id, content, ROUND((content <@> to_tpvector('database concurrent', 'concurrent_idx1'))::numeric, 4) as score
FROM concurrent_test_docs 
ORDER BY score DESC
LIMIT 5;

-- Test 3: Multiple index creation (simulates concurrent index builds)
\echo 'Test 3: Multiple index creation'

-- Create second table and index to test multiple indexes
INSERT INTO concurrent_test_docs2 (content, priority) VALUES
('high priority database tasks', 3),
('concurrent processing systems', 2),
('shared memory data structures', 1),
('database performance optimization', 3),
('concurrent system design patterns', 2);

CREATE INDEX concurrent_idx2 ON concurrent_test_docs2 USING tapir(content)
  WITH (text_config='english', k1=1.5, b=0.8);

-- Verify both indexes work independently
SELECT 'Table 1' as source, id, content
FROM concurrent_test_docs
UNION ALL
SELECT 'Table 2' as source, id, content
FROM concurrent_test_docs2
ORDER BY source, id;

-- Test 4: Stress test with many inserts
\echo 'Test 4: Bulk insert stress test'

-- Generate series of documents with overlapping terms
INSERT INTO concurrent_test_docs (content, category)
SELECT 
    'test document number ' || i || ' contains database and concurrent terms',
    'stress'
FROM generate_series(1, 50) i;

-- Verify search still works with larger dataset
SELECT COUNT(*) as total_matches
FROM concurrent_test_docs;

-- Test top results still make sense
SELECT id, substring(content, 1, 50) || '...' as content_preview, 
       ROUND((content <@> to_tpvector('database concurrent', 'concurrent_idx1'))::numeric, 4) as score
FROM concurrent_test_docs 
ORDER BY score DESC
LIMIT 8;

-- Test 5: Mixed operations (insert while searching)
\echo 'Test 5: Mixed insert and search operations'

-- This simulates a session inserting while another searches
BEGIN;
    INSERT INTO concurrent_test_docs (content, category) VALUES
    ('new concurrent database research paper', 'research');
    
    -- Search within same transaction
    SELECT id, content, ROUND((content <@> to_tpvector('research database', 'concurrent_idx1'))::numeric, 4) as score
    FROM concurrent_test_docs 
    ORDER BY score DESC
    LIMIT 3;
COMMIT;

-- Verify the insert is visible after commit
SELECT id, content, ROUND((content <@> to_tpvector('research database', 'concurrent_idx1'))::numeric, 4) as score
FROM concurrent_test_docs 
ORDER BY score DESC
LIMIT 3;

-- Test 6: Index integrity under updates
\echo 'Test 6: Update operations'

-- Update some documents to test posting list maintenance
UPDATE concurrent_test_docs 
SET content = 'updated database system with enhanced concurrent features'
WHERE id IN (1, 2);

-- Verify search finds updated documents
SELECT id, content, ROUND((content <@> to_tpvector('enhanced database', 'concurrent_idx1'))::numeric, 4) as score
FROM concurrent_test_docs 
ORDER BY score DESC;

-- Test 7: Delete operations
\echo 'Test 7: Delete operations'

-- Count before delete
SELECT COUNT(*) as count_before_delete
FROM concurrent_test_docs;

-- Delete some stress test documents
DELETE FROM concurrent_test_docs 
WHERE content LIKE 'test document number%' AND id % 5 = 0;

-- Count after delete to verify cleanup
SELECT COUNT(*) as count_after_delete
FROM concurrent_test_docs;

-- Test 8: Verify string interning consistency
\echo 'Test 8: String interning consistency'

-- Insert documents with exact same terms to test string interning
INSERT INTO concurrent_test_docs (content, category) VALUES
('exact same terms for testing consistency', 'test'),
('exact same terms for testing consistency', 'test'),
('testing consistency with exact same terms', 'test');

-- Search should find all variants
SELECT id, content, ROUND((content <@> to_tpvector('exact same terms', 'concurrent_idx1'))::numeric, 4) as score
FROM concurrent_test_docs 
ORDER BY score DESC, id;

-- Test 9: Multiple indexes on same table
\echo 'Test 9: Multiple indexes on same table'

-- Create a test table for multiple indexes
CREATE TABLE multi_idx_test (
    id SERIAL PRIMARY KEY,
    content TEXT NOT NULL
);

-- Insert initial data
INSERT INTO multi_idx_test (content) VALUES
('hello world from the test'),
('the quick brown fox jumps'),
('hello from another document');

-- Create two indexes with different text configurations
CREATE INDEX multi_idx_english ON multi_idx_test USING tapir(content)
  WITH (text_config='english', k1=1.2, b=0.75);

CREATE INDEX multi_idx_simple ON multi_idx_test USING tapir(content)
  WITH (text_config='simple', k1=1.5, b=0.8);

-- Insert more data after index creation
INSERT INTO multi_idx_test (content) VALUES
('world of databases and indexes'),
('hello database world');

-- Query using the English index
SELECT id, content, ROUND((content <@> to_tpvector('hello world', 'multi_idx_english'))::numeric, 4) as english_score
FROM multi_idx_test
ORDER BY english_score DESC;

-- Query using the Simple index
SELECT id, content, ROUND((content <@> to_tpvector('hello world', 'multi_idx_simple'))::numeric, 4) as simple_score
FROM multi_idx_test
ORDER BY simple_score DESC;

-- Verify both indexes exist and function independently
SELECT 
    i.indexrelid::regclass as index_name,
    pg_size_pretty(pg_relation_size(i.indexrelid)) as index_size
FROM pg_index i
JOIN pg_class c ON c.oid = i.indrelid
WHERE c.relname = 'multi_idx_test'
ORDER BY index_name;

-- Clean up test table
DROP TABLE multi_idx_test CASCADE;

-- Final verification
\echo 'Final verification: Index statistics'

SELECT 
    'concurrent_test_docs' as table_name,
    COUNT(*) as total_docs,
    COUNT(DISTINCT content) as unique_docs
FROM concurrent_test_docs
UNION ALL
SELECT 
    'concurrent_test_docs2' as table_name,
    COUNT(*) as total_docs,
    COUNT(DISTINCT content) as unique_docs
FROM concurrent_test_docs2;

-- Clean up
DROP TABLE concurrent_test_docs CASCADE;
DROP TABLE concurrent_test_docs2 CASCADE;