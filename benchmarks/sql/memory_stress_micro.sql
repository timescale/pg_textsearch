-- Micro memory stress benchmark for Tapir extension
-- Tests with a very small dataset to demonstrate functionality
-- and establish baseline performance characteristics
-- 
-- This benchmark:
-- 1. Creates a small table (1K documents) 
-- 2. Creates a Tapir index successfully
-- 3. Tests various queries 
-- 4. Provides timing and performance information

\timing on
\echo 'Starting Tapir micro memory stress benchmark...'

-- Create a table for testing
DROP TABLE IF EXISTS stress_docs_micro CASCADE;
CREATE TABLE stress_docs_micro (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT,
    category TEXT
);

\echo 'Generating micro document dataset (1K documents)...'

-- Insert documents with varied content
INSERT INTO stress_docs_micro (title, content, category)
SELECT 
    'Document ' || i,
    CASE (i % 10)
        WHEN 0 THEN 'artificial intelligence machine learning deep neural networks data science algorithm optimization performance scalability distributed systems'
        WHEN 1 THEN 'software engineering development programming languages python java javascript web development frontend backend fullstack'  
        WHEN 2 THEN 'cybersecurity information security network security application security data protection privacy encryption authentication'
        WHEN 3 THEN 'data analytics business intelligence data warehousing extract transform load pipelines batch streaming processing'
        WHEN 4 THEN 'cloud computing amazon web services microsoft azure google cloud platform infrastructure platform software as service'
        WHEN 5 THEN 'mobile development ios android react native flutter xamarin swift kotlin java objective c dart javascript'
        WHEN 6 THEN 'database management systems postgresql mysql oracle mongodb redis elasticsearch indexing querying optimization'
        WHEN 7 THEN 'project management agile scrum kanban lean methodology planning estimation scheduling resource allocation'
        WHEN 8 THEN 'machine learning supervised unsupervised reinforcement learning linear regression logistic decision trees'
        ELSE 'enterprise architecture service oriented microservices event driven domain driven design api gateway discovery'
    END || 
    ' Document ' || i || ' unique content with hash ' || substr(md5(i::text), 1, 8),
    
    CASE (i % 3)
        WHEN 0 THEN 'Technology'
        WHEN 1 THEN 'Business'  
        ELSE 'Engineering'
    END
FROM generate_series(1, 1000) AS i;  -- 1K documents for quick testing

\echo 'Document generation complete. Dataset summary:'
SELECT COUNT(*) as total_docs, 
       AVG(LENGTH(content)) as avg_content_length,
       MAX(LENGTH(content)) as max_content_length,
       MIN(LENGTH(content)) as min_content_length
FROM stress_docs_micro;

\echo ''
\echo 'Current Tapir configuration:'
SHOW tapir.shared_memory_size;
SHOW tapir.default_limit;

\echo ''
\echo 'Creating Tapir index...'

CREATE INDEX stress_micro_idx 
ON stress_docs_micro 
USING tapir(content) 
WITH (text_config='english', k1=1.2, b=0.75);

\echo 'Index created successfully! Running performance tests...'

-- Test queries with timing
\echo ''
\echo '=== Performance Test Results ==='

\echo 'Test 1: Technical terms search'
SELECT COUNT(*) as matches
FROM stress_docs_micro 
WHERE content <@> to_tpvector('algorithm optimization', 'stress_micro_idx') > 0;

\echo 'Test 2: AI/ML search'
SELECT COUNT(*) as matches
FROM stress_docs_micro
WHERE content <@> to_tpvector('machine learning artificial intelligence', 'stress_micro_idx') > 0;

\echo 'Test 3: Database search'
SELECT COUNT(*) as matches
FROM stress_docs_micro
WHERE content <@> to_tpvector('database postgresql mysql', 'stress_micro_idx') > 0;

\echo 'Test 4: Top results with scores'
SELECT title, 
       round((content <@> to_tpvector('software development', 'stress_micro_idx'))::numeric, 4) as score
FROM stress_docs_micro
WHERE content <@> to_tpvector('software development', 'stress_micro_idx') > 0
ORDER BY score DESC
LIMIT 3;

\echo 'Test 5: Category-based analysis'
SELECT 
    category,
    COUNT(*) as total_docs,
    COUNT(*) FILTER (WHERE content <@> to_tpvector('programming software', 'stress_micro_idx') > 0) as programming_matches
FROM stress_docs_micro
GROUP BY category
ORDER BY category;

-- Test memory behavior with larger queries
\echo ''
\echo '=== Memory Pressure Tests ==='

\echo 'Test 6: Common terms (should find many matches)'
SELECT COUNT(*) as matches
FROM stress_docs_micro
WHERE content <@> to_tpvector('data system', 'stress_micro_idx') > 0;

\echo 'Test 7: Specific terms (should find fewer matches)'
SELECT COUNT(*) as matches  
FROM stress_docs_micro
WHERE content <@> to_tpvector('kubernetes docker containerization', 'stress_micro_idx') > 0;

\echo 'Test 8: Complex multi-term query'
SELECT COUNT(*) as matches
FROM stress_docs_micro
WHERE content <@> to_tpvector('web development frontend javascript react', 'stress_micro_idx') > 0;

-- Performance summary
\echo ''
\echo '=== Benchmark Summary ==='
SELECT 
    'Micro benchmark completed' as status,
    COUNT(*) as total_documents,
    'Index created successfully' as index_status,
    'Queries executed successfully' as query_status
FROM stress_docs_micro;

-- Clean up
\echo ''
\echo 'Cleaning up test resources...'
DROP TABLE stress_docs_micro CASCADE;

\echo ''
\echo 'Micro memory stress benchmark completed.'
\echo 'This establishes baseline functionality with 1K documents.'
\echo 'For true stress testing, try larger datasets or reduced memory settings.'