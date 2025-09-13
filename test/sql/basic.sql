-- Basic functionality tests for tapir extension

-- Test extension creation
CREATE EXTENSION IF NOT EXISTS tapir;

-- Test tpvector type exists
SELECT pg_typeof('my_index:{database:2,system:1}'::tpvector);

-- Test tpvector input/output
SELECT 'my_index:{database:2,system:1}'::tpvector;

-- Test empty tpvector
SELECT 'my_index:{}'::tpvector;

-- Test tapir access method exists
SELECT amname FROM pg_am WHERE amname = 'tapir';

-- Test creating a tapir index with text_config
CREATE TABLE test_docs (id SERIAL PRIMARY KEY, content TEXT);
CREATE INDEX test_tapir_idx ON test_docs USING tapir(content) WITH (text_config='english');

-- Verify index was created
SELECT indexrelid::regclass FROM pg_index 
WHERE indrelid = 'test_docs'::regclass 
AND indexrelid::regclass::text LIKE '%tapir%';

-- Clean up
DROP TABLE test_docs CASCADE;