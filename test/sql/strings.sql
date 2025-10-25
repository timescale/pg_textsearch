-- Test long string handling including URLs, paths, and long terms
-- Load pg_textsearch extension (suppress output for test isolation)
SET client_min_messages = WARNING;
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
RESET client_min_messages;

-- Enable score logging for testing
SET pg_textsearch.log_scores = true;
SET client_min_messages = NOTICE;
SET enable_seqscan = false;

-- Setup test table for long strings
DROP TABLE IF EXISTS long_string_docs CASCADE;
CREATE TABLE long_string_docs (
    id SERIAL PRIMARY KEY,
    content TEXT,
    category VARCHAR(50)
);

-- Insert test data with various long string patterns
INSERT INTO long_string_docs (content, category) VALUES
    -- URLs and web content
    ('Visit our website at https://www.example-company.com/products/database-solutions/postgresql-extensions/full-text-search?utm_source=docs&utm_medium=referral&utm_campaign=tapir-extension for more information about BM25 indexing', 'web'),
    ('API endpoint: https://api.search-service.example.org/v2/documents/search/bm25?query=full+text+search&limit=100&offset=0&include_scores=true&config=english', 'api'),
    ('Documentation available at https://docs.postgresql-extensions.community/tapir/getting-started/installation-guide.html#shared-memory-configuration', 'docs'),

    -- Long file paths and system content
    ('Error in file /usr/local/lib/postgresql/extensions/tapir/src/posting-list-management/shared-memory/string-interning-hashtable.c line 1547', 'system'),
    ('Log file: /var/log/postgresql/tapir-extension/performance-monitoring/bm25-scoring-statistics/daily-reports/2024-08-27-full-text-search-analytics.log', 'logs'),

    -- Long technical terms and identifiers
    ('PostgreSQL extension configuration parameter shared_preload_libraries_tapir_bm25_full_text_search_with_relevance_scoring_enabled', 'config'),
    ('Function name: tp_calculate_bm25_relevance_score_with_term_frequency_inverse_document_frequency_and_document_length_normalization', 'technical'),

    -- Mixed long content with URLs embedded
    ('Our advanced information retrieval system uses BM25 scoring algorithm implementation available at https://github.com/enterprise-search/tapir-postgresql-extension/blob/main/src/algorithms/bm25-relevance-scoring.c for processing full-text search queries', 'mixed'),

    -- Very long single terms (scientific/chemical names)
    ('The antidisestablishmentarianism movement was characterized by pneumonoultramicroscopicsilicovolcanoconiosispneumonoultramicroscopicsilicovolcanoconiosis research', 'scientific'),

    -- Long JSON-like structured content
    ('Configuration: {"search_engine":"tapir","algorithms":{"primary":"bm25","parameters":{"k1":1.2,"b":0.75}},"text_processing":{"stemming":"english","tokenization":"standard"},"performance":{"shared_memory_mb":64,"max_terms":1000000}}', 'json');

-- Create pg_textsearch index on long strings
CREATE INDEX long_strings_idx ON long_string_docs USING bm25(content)
    WITH (text_config='english', k1=1.2, b=0.75);

-- Test 1: Search for URL components
SELECT id, LEFT(content, 80) || '...' as content_preview,
       ROUND((content <@> to_bm25query('https website', 'long_strings_idx'))::numeric, 4) as score
FROM long_string_docs
ORDER BY content <@> to_bm25query('https website', 'long_strings_idx')
LIMIT 5;

-- Test 2: Search for technical terms
SELECT * FROM (
    SELECT id, category,
           ROUND((content <@> to_bm25query('postgresql extension', 'long_strings_idx'))::numeric, 4) as score
    FROM long_string_docs
    ORDER BY content <@> to_bm25query('postgresql extension', 'long_strings_idx')
    LIMIT 10
) AS subquery
ORDER BY score, id;

-- Test 3: Search for long path components
SELECT * FROM (
    SELECT id, LEFT(content, 60) || '...' as content_preview,
           ROUND((content <@> to_bm25query('file log error', 'long_strings_idx'))::numeric, 4) as score
    FROM long_string_docs
    ORDER BY content <@> to_bm25query('file log error', 'long_strings_idx')
    LIMIT 10
) AS subquery
ORDER BY score, id;

-- Test 4: Test vectorization of very long URLs
SELECT to_bm25vector('https://www.very-long-domain-name-for-testing-url-tokenization.example.com/extremely/long/path/with/many/segments/and/parameters?param1=value1&param2=value2&param3=value3', 'long_strings_idx');

-- Test 5: Test extremely long single term handling
SELECT to_bm25vector('supercalifragilisticexpialidociouspneumonoultramicroscopicsilicovolcanoconiosisantidisestablishmentarianism', 'long_strings_idx');

-- Test 6: Test mixed long and short terms
SELECT * FROM (
    SELECT id, ROUND((content <@> to_bm25query('algorithm bm25', 'long_strings_idx'))::numeric, 4) as score
    FROM long_string_docs
    ORDER BY content <@> to_bm25query('algorithm bm25', 'long_strings_idx')
    LIMIT 10
) AS subquery
ORDER BY score, id;

-- Test 7: Performance test with multiple long term queries
SELECT * FROM (
    SELECT
        id,
        category,
        ROUND((content <@> to_bm25query('postgresql pg_textsearch extension search', 'long_strings_idx'))::numeric, 4) as multi_term_score
    FROM long_string_docs
    ORDER BY content <@> to_bm25query('postgresql pg_textsearch extension search', 'long_strings_idx')
    LIMIT 10
) AS subquery
ORDER BY multi_term_score, id;

-- Test 8: Test URL tokenization specifics
SELECT
    'URL components' as test_type,
    to_bm25vector('https://example.com/path?param=value', 'long_strings_idx') as url_vector;

-- Test 9: Test that very long terms don't cause memory issues
INSERT INTO long_string_docs (content, category) VALUES
    ('This document contains a ridiculously long compound word: ' ||
     'antidisestablishmentarianismpneumonoultramicroscopicsilicovolcanoconiosis' ||
     'supercalifragilisticexpialidociousfloccinaucinihilipilificationhippopotomonstrosesquippedaliophobia' ||
     ' and should still be indexed correctly', 'stress_test');

-- Verify the stress test document was indexed
SELECT * FROM (
    SELECT id, LEFT(content, 50) || '...' as preview,
           ROUND((content <@> to_bm25query('document ridiculously long', 'long_strings_idx'))::numeric, 4) as score
    FROM long_string_docs
    ORDER BY content <@> to_bm25query('document ridiculously long', 'long_strings_idx')
    LIMIT 10
) AS subquery
ORDER BY score, id;

-- Test 10: Memory usage statistics after indexing long strings
SELECT
    COUNT(*) as total_documents,
    AVG(LENGTH(content)) as avg_content_length,
    MAX(LENGTH(content)) as max_content_length,
    COUNT(CASE WHEN LENGTH(content) > 200 THEN 1 END) as long_documents,
    COUNT(CASE WHEN LENGTH(content) > 500 THEN 1 END) as very_long_documents
FROM long_string_docs;

-- Test 11: Test stack/heap threshold boundary (1KB limit for alloca)
INSERT INTO long_string_docs (content, category) VALUES
    -- Just under 1KB threshold (should use stack allocation)
    ('Short term: ' || REPEAT('stackallocation', 40), 'stack_test'),
    -- Just over 1KB threshold (should use heap allocation)
    ('Long term: ' || REPEAT('heapallocationfallback', 50), 'heap_test'),
    -- Very long content with mixed term lengths
    ('Mixed content with ' || REPEAT('shortterm ', 20) || ' and ' ||
     REPEAT('verylongtermnameforheapallocationtestingpurposes', 30) ||
     ' to test both stack and heap allocation paths in the same document', 'mixed_allocation');

-- Verify threshold test documents can be searched
SELECT
    id,
    category,
    LENGTH(content) as content_length,
    CASE
        WHEN LENGTH(content) < 1000 THEN 'Stack allocation likely'
        ELSE 'Heap allocation likely'
    END as allocation_type,
    ROUND((content <@> to_bm25query('term allocation', 'long_strings_idx'))::numeric, 4) as score
FROM long_string_docs
WHERE category IN ('stack_test', 'heap_test', 'mixed_allocation')
    AND content <@> to_bm25query('term allocation', 'long_strings_idx') < 0
ORDER BY content_length;

-- Test 12: Extreme length document (tests PostgreSQL's text limit handling)
INSERT INTO long_string_docs (content, category) VALUES
    ('Extreme test: ' || REPEAT('This is a very long repeated phrase for testing PostgreSQL text limits and our hybrid stack heap allocation strategy. ', 200), 'extreme');

-- Verify extreme length document indexing
SELECT
    'Extreme length test' as test_name,
    LENGTH(content) as total_length,
    ROUND((content <@> to_bm25query('extreme testing', 'long_strings_idx'))::numeric, 4) as score
FROM long_string_docs
WHERE category = 'extreme'
    AND content <@> to_bm25query('extreme testing', 'long_strings_idx') < 0;

-- Cleanup
DROP TABLE long_string_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
