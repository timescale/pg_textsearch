-- Test repeated index creation/dropping to check for DSA leaks
CREATE EXTENSION IF NOT EXISTS pgtextsearch;

-- Run 10 cycles of create/drop
DO $$
DECLARE
    i int;
BEGIN
    FOR i IN 1..10 LOOP
        -- Create table
        CREATE TABLE test_table (id serial PRIMARY KEY, content text);
        INSERT INTO test_table (content) VALUES ('test document ' || i);

        -- Create index
        EXECUTE 'CREATE INDEX idx_test ON test_table USING bm25(content) WITH (text_config=''english'')';

        -- Drop table (cascades to index)
        DROP TABLE test_table CASCADE;

        RAISE NOTICE 'Completed cycle %', i;
    END LOOP;
END $$;

SELECT 'Test completed' as result;
