-- Debug why "system" isn't indexed
DROP EXTENSION IF EXISTS tapir CASCADE;
CREATE EXTENSION tapir;

CREATE TABLE test_doc (id int, content text);
INSERT INTO test_doc VALUES (1, 'database management system');

-- Create index with logging enabled
SET client_min_messages = NOTICE;
CREATE INDEX test_idx ON test_doc USING tapir(content) WITH (text_config='english');

-- Check what's in the index
SELECT tp_debug_dump_index('test_idx');

DROP TABLE test_doc CASCADE;
