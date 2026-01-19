-- Test parameter "text_config" accepts qualified names
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create table
CREATE TABLE text_config_tbl (
    id SERIAL PRIMARY KEY,
    content TEXT
);

-- Verify that we can use qualified names when specifying "text_config"
-- and the text configuration can be found by Postgres
CREATE INDEX text_config_idx1 ON text_config_tbl USING bm25 (content) 
    WITH (text_config = 'pg_catalog.english', k1=1.2, b=0.75);

DROP TABLE text_config_tbl;
DROP EXTENSION pg_textsearch CASCADE;