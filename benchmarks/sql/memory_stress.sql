-- Memory stress benchmark for Tapir extension
-- Tests behavior when shared memory is exhausted with large document dataset
--
-- This benchmark:
-- 1. Sets a very small shared memory limit (1MB)
-- 2. Creates a large table with randomly generated documents
-- 3. Attempts to create a Tapir index, expecting failure due to memory limits
-- 4. Demonstrates the extension's memory exhaustion behavior

\echo 'Starting Tapir memory stress benchmark...'

-- Note: Since shared_memory_size is PGC_POSTMASTER, it requires a server restart
-- For testing purposes, we'll use a large dataset to stress the current memory allocation
\echo 'Note: To test with minimal memory, set tapir.shared_memory_size=1 in postgresql.conf and restart PostgreSQL'
\echo 'Current test will use large dataset to stress existing memory allocation'

-- Create a table for stress testing
DROP TABLE IF EXISTS stress_docs CASCADE;
CREATE TABLE stress_docs (
    id SERIAL PRIMARY KEY,
    title TEXT,
    content TEXT,
    category TEXT,
    tags TEXT[]
);

\echo 'Generating large document dataset...'

-- Insert documents with randomly generated content
-- Using various content patterns to create realistic text diversity
INSERT INTO stress_docs (title, content, category, tags)
SELECT
    'Document ' || i,
    -- Generate varied content lengths and vocabularies
    CASE (i % 10)
        WHEN 0 THEN 'artificial intelligence machine learning deep neural networks data science algorithm optimization performance scalability distributed systems cloud computing microservices containerization kubernetes docker database postgresql indexing search ranking relevance information retrieval natural language processing text mining sentiment analysis classification clustering regression prediction model training validation testing deployment monitoring logging metrics analytics big data hadoop spark kafka elasticsearch mongodb redis caching memory optimization query performance database design normalization denormalization sharding replication consistency availability partition tolerance CAP theorem ACID transactions isolation levels concurrency control deadlock prevention'
        WHEN 1 THEN 'software engineering development programming languages python java javascript typescript go rust c plus plus scala kotlin swift objective c ruby php perl shell bash scripting automation testing unit integration system performance load stress testing continuous integration deployment devops infrastructure as code terraform ansible chef puppet monitoring alerting logging observability tracing metrics dashboards grafana prometheus elk stack version control git github gitlab bitbucket code review pull requests merge conflicts branching strategies gitflow feature branches hotfixes releases semantic versioning'
        WHEN 2 THEN 'web development frontend backend fullstack html css javascript typescript react vue angular node express django flask spring boot ruby rails php laravel codeigniter symfony asp net mvc api rest graphql soap microservices architecture patterns mvc mvp mvvm observer factory singleton repository dependency injection inversion of control aspect oriented programming functional programming object oriented programming design patterns solid principles clean code refactoring technical debt documentation'
        WHEN 3 THEN 'cybersecurity information security network security application security data protection privacy encryption decryption hashing salting authentication authorization access control identity management single sign on multi factor authentication oauth jwt tokens vulnerability assessment penetration testing security auditing compliance regulations gdpr hipaa sox pci dss threat modeling risk assessment incident response disaster recovery business continuity backup strategies data retention policies'
        WHEN 4 THEN 'data analytics business intelligence data warehousing extract transform load etl data pipelines batch streaming processing apache airflow luigi prefect data quality data governance master data management metadata lineage cataloging discovery profiling cleansing standardization validation enrichment aggregation reporting dashboards visualization tableau power bi looker qlik sense statistical analysis hypothesis testing confidence intervals correlation causation regression classification clustering'
        WHEN 5 THEN 'cloud computing amazon web services microsoft azure google cloud platform infrastructure platform software as a service compute storage networking content delivery edge computing serverless functions containers orchestration kubernetes docker swarm service mesh istio consul vault secrets management configuration management infrastructure as code terraform cloudformation arm templates cost optimization resource management auto scaling load balancing high availability disaster recovery'
        WHEN 6 THEN 'mobile development ios android react native flutter xamarin cordova phonegap ionic swift kotlin java objective c dart javascript html5 css3 responsive design progressive web apps pwa service workers offline support push notifications app store optimization aso user experience user interface design wireframing prototyping testing debugging performance optimization battery usage memory management network efficiency security encryption data protection privacy'
        WHEN 7 THEN 'project management agile scrum kanban lean six sigma waterfall methodology planning estimation scheduling resource allocation risk management stakeholder communication requirements gathering user stories acceptance criteria sprint planning daily standups retrospectives product owner scrum master project manager business analyst quality assurance testing documentation change management scope creep timeline milestones deliverables'
        WHEN 8 THEN 'artificial intelligence machine learning supervised unsupervised reinforcement learning linear regression logistic regression decision trees random forest support vector machines neural networks convolutional recurrent transformer attention mechanisms gradient descent backpropagation optimization algorithms feature engineering selection extraction dimensionality reduction principal component analysis cross validation hyperparameter tuning model evaluation metrics accuracy precision recall f1 score confusion matrix'
        ELSE 'enterprise architecture service oriented architecture microservices event driven architecture domain driven design bounded contexts aggregate root entity value object repository pattern command query responsibility segregation event sourcing saga pattern orchestration choreography api gateway service discovery circuit breaker bulkhead pattern retry timeout exponential backoff graceful degradation health checks monitoring alerting distributed tracing logging correlation ids'
    END ||
    ' Additional content block ' || i || ' with unique identifier ' || md5(i::text) ||
    ' More varied text to increase vocabulary diversity and term frequency distribution across documents.',

    CASE (i % 5)
        WHEN 0 THEN 'Technology'
        WHEN 1 THEN 'Science'
        WHEN 2 THEN 'Business'
        WHEN 3 THEN 'Engineering'
        ELSE 'Research'
    END,

    ARRAY[
        CASE (i % 8)
            WHEN 0 THEN 'programming'
            WHEN 1 THEN 'database'
            WHEN 2 THEN 'algorithm'
            WHEN 3 THEN 'optimization'
            WHEN 4 THEN 'performance'
            WHEN 5 THEN 'scalability'
            WHEN 6 THEN 'architecture'
            ELSE 'development'
        END,
        'tag' || (i % 20),
        'category' || (i % 10)
    ]
FROM generate_series(1, 100000) AS i;  -- 100K documents to stress memory

\echo 'Document generation complete. Table contains:'
SELECT COUNT(*) as total_docs,
       AVG(LENGTH(content)) as avg_content_length,
       MAX(LENGTH(content)) as max_content_length
FROM stress_docs;

CREATE INDEX stress_content_idx
ON stress_docs
USING pg_textsearch(content)
WITH (text_config='english', k1=1.2, b=0.75);

-- If we get here, let's try some queries to stress the system further
\echo 'Index creation succeeded! Testing query performance under memory pressure...'

-- Try various queries to stress the system
SELECT 'Query 1: Common terms' as test_name;
SELECT COUNT(*) FROM (
    SELECT 1
    FROM stress_docs
    ORDER BY content <@> to_tpquery('algorithm optimization performance', 'stress_content_idx')
    LIMIT 100
) subq;

SELECT 'Query 2: Technical terms' as test_name;
SELECT COUNT(*) FROM (
    SELECT 1
    FROM stress_docs
    ORDER BY content <@> to_tpquery('machine learning artificial intelligence', 'stress_content_idx')
    LIMIT 100
) subq;

SELECT 'Query 3: Specific terms' as test_name;
SELECT COUNT(*) FROM (
    SELECT 1
    FROM stress_docs
    ORDER BY content <@> to_tpquery('postgresql database indexing search', 'stress_content_idx')
    LIMIT 100
) subq;

\echo 'Query 4: Large result set to stress memory allocation'
SELECT COUNT(*) FROM (
    SELECT 1
    FROM stress_docs
    ORDER BY content <@> to_tpquery('the and of to in', 'stress_content_idx')
    LIMIT 10000
) subq;

-- Clean up
\echo ''
\echo 'Cleaning up stress test resources...'
DROP TABLE IF EXISTS stress_docs CASCADE;

-- Note: Memory settings are PGC_POSTMASTER and require restart to change

\echo ''
\echo 'Memory stress benchmark completed.'
\echo 'Check the output above for memory exhaustion behavior and performance characteristics.'
