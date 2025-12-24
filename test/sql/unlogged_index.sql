-- Test unlogged index behavior and initialization fork creation
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Create UNLOGGED table and index
CREATE UNLOGGED TABLE unlogged_test (
    id SERIAL PRIMARY KEY,
    content TEXT
);

INSERT INTO unlogged_test (content) VALUES ('test document');

CREATE INDEX unlogged_idx ON unlogged_test USING bm25(content) 
WITH (text_config='english', k1=1.2, b=0.75);

-- VERIFICATION: Check if _init fork exists and has valid size
--
-- We verify that the init fork contains at least the meta page
SELECT 
    relname, 
    -- Main fork should have data (Metapage + DocID page + etc)
    pg_relation_size(indexrelid, 'main') > current_setting('block_size')::int as main_has_data,
    -- Init fork must exist and be at least one page size
    CASE 
        WHEN pg_relation_size(indexrelid, 'init') >= current_setting('block_size')::int 
        THEN 'PASS' 
        ELSE 'FAIL' 
    END as init_check
FROM pg_index i
JOIN pg_class c ON c.oid = i.indexrelid
WHERE c.relname = 'unlogged_idx';

-- Cleanup
DROP TABLE unlogged_test;
DROP EXTENSION pg_textsearch CASCADE;