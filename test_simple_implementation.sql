-- Simple test of new shared memory implementation
DROP TABLE IF EXISTS simple_test CASCADE;
DROP EXTENSION IF EXISTS tapir CASCADE;

CREATE EXTENSION tapir;

-- Test basic functionality
CREATE TABLE simple_test (id SERIAL PRIMARY KEY, content TEXT);

-- Insert test data
INSERT INTO simple_test (content) VALUES
    ('hello world example');

-- Create index
CREATE INDEX simple_idx ON simple_test USING tapir(content) WITH (text_config='english');

-- Test scoring
SELECT content, content <@> to_tpquery('hello', 'simple_idx') as score
FROM simple_test
WHERE content <@> to_tpquery('hello', 'simple_idx') > 0;

-- Clean up
DROP TABLE simple_test CASCADE;
DROP EXTENSION tapir CASCADE;