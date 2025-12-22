-- Test behavior when using a dropped index name in queries

-- Ensure extension is loaded
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create test table
CREATE TABLE dropped_idx_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Insert test documents
INSERT INTO dropped_idx_test (content) VALUES
    ('first test document'),
    ('second test document'),
    ('third test document'),
    ('fourth test document'),
    ('fifth test document');

-- Create index
CREATE INDEX dropped_idx ON dropped_idx_test
USING bm25 (content)
WITH (text_config = 'english');

-- Query should work with index (using explicit index reference)
SELECT COUNT(*) AS with_index
FROM dropped_idx_test
WHERE content <@> to_bm25query('test', 'dropped_idx') < -0.001;

-- Drop the index
DROP INDEX dropped_idx;

-- Query should ERROR when index doesn't exist
SELECT COUNT(*) AS after_drop
FROM dropped_idx_test
WHERE content <@> to_bm25query('test', 'dropped_idx') < -0.001;

-- Also test with a completely non-existent index name
SELECT COUNT(*) AS nonexistent
FROM dropped_idx_test
WHERE content <@> to_bm25query('test', 'totally_fake_index') < -0.001;

-- Test registry slot cleanup (issue #83)
-- Create multiple indexes, drop them, then create new ones
-- This verifies that registry slots are properly freed on drop

-- Create 5 indexes
CREATE INDEX dropped_idx_1 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');
CREATE INDEX dropped_idx_2 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');
CREATE INDEX dropped_idx_3 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');
CREATE INDEX dropped_idx_4 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');
CREATE INDEX dropped_idx_5 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');

-- Drop all 5
DROP INDEX dropped_idx_1;
DROP INDEX dropped_idx_2;
DROP INDEX dropped_idx_3;
DROP INDEX dropped_idx_4;
DROP INDEX dropped_idx_5;

-- Create 5 more indexes (should succeed if slots were freed)
CREATE INDEX dropped_idx_6 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');
CREATE INDEX dropped_idx_7 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');
CREATE INDEX dropped_idx_8 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');
CREATE INDEX dropped_idx_9 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');
CREATE INDEX dropped_idx_10 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');

-- Test that the new indexes work
SELECT COUNT(*) AS recycled_slots_work
FROM dropped_idx_test
WHERE content <@> to_bm25query('test', 'dropped_idx_6') < -0.001;

-- Also test CASCADE drop (table drop should clean up index slots)
CREATE TABLE cascade_test (id SERIAL, content TEXT);
INSERT INTO cascade_test (content) VALUES ('cascade test doc');
CREATE INDEX cascade_idx ON cascade_test USING bm25 (content) WITH (text_config = 'english');

-- Drop table should cascade to drop index and free its registry slot
DROP TABLE cascade_test CASCADE;

-- Create another index to verify the cascade-dropped slot was freed
CREATE INDEX dropped_idx_11 ON dropped_idx_test USING bm25 (content) WITH (text_config = 'english');

-- Clean up
DROP TABLE dropped_idx_test;
DROP EXTENSION pg_textsearch;
