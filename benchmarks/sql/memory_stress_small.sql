-- Smaller memory stress benchmark for Tapir extension
-- Tests behavior with a more manageable dataset size
--
-- This benchmark:
-- 1. Creates a moderately large table (25K documents)
-- 2. Creates a Tapir index to demonstrate current performance
-- 3. Tests various queries to show system behavior
-- 4. Provides timing information

\timing on
\echo 'Starting Tapir memory stress benchmark (small version)...'

-- Create a table for stress testing
DROP TABLE IF EXISTS stress_docs_small CASCADE;
CREATE TABLE stress_docs_small (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT,
    category TEXT,
    tags TEXT[]
);

\echo 'Generating document dataset (25K documents)...'

-- Insert documents with randomly generated content
-- Using various content patterns to create realistic text diversity
INSERT INTO stress_docs_small (title, content, category, tags)
SELECT
    'Document ' || i,
    -- Generate varied content lengths and vocabularies (shorter for faster testing)
    CASE (i % 5)
        WHEN 0 THEN 'artificial intelligence machine learning deep neural networks data science algorithm optimization performance scalability distributed systems cloud computing database postgresql indexing search ranking relevance information retrieval natural language processing text mining sentiment analysis'
        WHEN 1 THEN 'software engineering development programming languages python java javascript web development frontend backend fullstack testing continuous integration deployment devops infrastructure monitoring observability'
        WHEN 2 THEN 'cybersecurity information security network security application security data protection privacy encryption authentication authorization vulnerability assessment penetration testing security auditing'
        WHEN 3 THEN 'data analytics business intelligence data warehousing extract transform load pipelines batch streaming processing statistical analysis hypothesis testing correlation regression classification clustering'
        ELSE 'mobile development cloud computing project management agile scrum enterprise architecture microservices event driven domain driven design api gateway service discovery circuit breaker'
    END ||
    ' Document identifier ' || i || ' with hash ' || substr(md5(i::text), 1, 8) ||
    ' Additional unique content for vocabulary diversity.',

    CASE (i % 3)
        WHEN 0 THEN 'Technology'
        WHEN 1 THEN 'Business'
        ELSE 'Engineering'
    END,

    ARRAY[
        'tag' || (i % 10),
        'category' || (i % 5),
        CASE (i % 4)
            WHEN 0 THEN 'programming'
            WHEN 1 THEN 'database'
            WHEN 2 THEN 'algorithm'
            ELSE 'optimization'
        END
    ]
FROM generate_series(1, 25000) AS i;  -- 25K documents for reasonable test time

\echo 'Document generation complete. Table contains:'
SELECT COUNT(*) as total_docs,
       AVG(LENGTH(content)) as avg_content_length,
       MAX(LENGTH(content)) as max_content_length,
       MIN(LENGTH(content)) as min_content_length
FROM stress_docs_small;


\echo ''
\echo 'Creating Tapir index on content column...'

CREATE INDEX stress_content_small_idx
ON stress_docs_small
USING tapir(content)
WITH (text_config='english', k1=1.2, b=0.75);

\echo 'Index creation completed! Running performance queries...'

-- Test queries with timing
\echo ''
\echo '=== Query Performance Tests ==='

\echo 'Query 1: Common technical terms'
SELECT COUNT(*) as matches
FROM stress_docs_small
WHERE content <@> to_tpquery('algorithm optimization performance', 'stress_content_small_idx') < 0;

\echo 'Query 2: AI/ML terms'
SELECT COUNT(*) as matches
FROM stress_docs_small
WHERE content <@> to_tpquery('machine learning artificial intelligence', 'stress_content_small_idx') < 0;

\echo 'Query 3: Database terms'
SELECT COUNT(*) as matches
FROM stress_docs_small
WHERE content <@> to_tpquery('database postgresql indexing search', 'stress_content_small_idx') < 0;

\echo 'Query 4: Top scoring results'
SELECT title,
       content <@> to_tpquery('software development programming', 'stress_content_small_idx') as score
FROM stress_docs_small
WHERE content <@> to_tpquery('software development programming', 'stress_content_small_idx') < 0
ORDER BY score DESC
LIMIT 5;

\echo 'Query 5: Multiple different searches'
SELECT
    'AI/ML' as search_type,
    COUNT(*) as matches
FROM stress_docs_small
WHERE content <@> to_tpquery('artificial intelligence machine learning', 'stress_content_small_idx') < 0

UNION ALL

SELECT
    'Security' as search_type,
    COUNT(*) as matches
FROM stress_docs_small
WHERE content <@> to_tpquery('security encryption authentication', 'stress_content_small_idx') < 0

UNION ALL

SELECT
    'Development' as search_type,
    COUNT(*) as matches
FROM stress_docs_small
WHERE content <@> to_tpquery('programming development software', 'stress_content_small_idx') < 0;

-- Index statistics
\echo ''
\echo '=== Index Statistics ==='
SELECT
    schemaname,
    tablename,
    indexname,
    tablespace,
    indexdef
FROM pg_indexes
WHERE indexname = 'stress_content_small_idx';

-- Clean up
\echo ''
\echo 'Cleaning up test resources...'
DROP TABLE stress_docs_small CASCADE;

\echo ''
\echo 'Small memory stress benchmark completed successfully.'
\echo 'Check timing information above to assess performance characteristics.'
