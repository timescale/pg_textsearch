-- Set up benchmark_queries_power table for pgbench power tests.
-- Remaps sparse MS-MARCO query_ids to dense integers 1..N so that
-- pgbench can use \set qid random(1, N).

\set ON_ERROR_STOP on

-- Idempotent: recreate every time
DROP TABLE IF EXISTS benchmark_queries_power;

-- Staging table with original columns
CREATE TEMP TABLE _bq_raw (
    query_id     INTEGER,
    query_text   TEXT,
    token_bucket INTEGER
);
\copy _bq_raw FROM 'benchmarks/datasets/msmarco-v2/benchmark_queries.tsv' WITH (FORMAT text, DELIMITER E'\t')

-- Dense sequential IDs 1..N
CREATE TABLE benchmark_queries_power (
    id          INTEGER PRIMARY KEY,
    query_text  TEXT NOT NULL
);

INSERT INTO benchmark_queries_power (id, query_text)
SELECT row_number() OVER (ORDER BY query_id)::int, query_text
FROM _bq_raw;

DROP TABLE _bq_raw;

-- Verify
SELECT 'Power test queries loaded: ' || COUNT(*) AS status
FROM benchmark_queries_power;
SELECT 'ID range: ' || MIN(id) || '..' || MAX(id) AS range
FROM benchmark_queries_power;
