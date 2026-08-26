CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\pset format unaligned
SET client_min_messages = warning;
SET pg_textsearch.segments_per_level = 64;
SET pg_textsearch.compaction_mode = 'background';

CREATE TABLE compaction_log (idx regclass);

CREATE FUNCTION log_compaction(idx regclass) RETURNS void
LANGUAGE sql AS $$
    INSERT INTO compaction_log VALUES (idx);
$$;

CREATE FUNCTION fail_compaction(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'intentional request failure for %', idx;
END;
$$;

CREATE TABLE compaction_request_docs (
    id serial PRIMARY KEY,
    body text
);
CREATE INDEX compaction_request_docs_idx ON compaction_request_docs
    USING bm25(body) WITH (text_config = 'english');

CREATE TABLE compaction_request_docs2 (
    id serial PRIMARY KEY,
    body text
);
CREATE INDEX compaction_request_docs2_idx ON compaction_request_docs2
    USING bm25(body) WITH (text_config = 'english');

SET pg_textsearch.compaction_request_function = 'log_compaction';

-- Rollback leaves no log rows because the dispatch is transaction-local.
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'rollback doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS rollback_spill;
ROLLBACK;
SELECT count(*) AS rollback_log_rows FROM compaction_log;

-- Commit dispatches one request atomically with the writer transaction.
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'commit doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS commit_spill;
COMMIT;
SELECT count(*) AS commit_log_rows
FROM compaction_log
WHERE idx = 'compaction_request_docs_idx'::regclass;

-- Two spills for the same index in one transaction are deduplicated.
TRUNCATE compaction_log;
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'dedup first doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS dedup_spill1;
INSERT INTO compaction_request_docs (body)
SELECT 'dedup second doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS dedup_spill2;
COMMIT;
SELECT count(*) AS dedup_log_rows
FROM compaction_log
WHERE idx = 'compaction_request_docs_idx'::regclass;

-- Different indexes in one transaction each dispatch once.
TRUNCATE compaction_log;
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'multi first doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS multi_spill1;
INSERT INTO compaction_request_docs2 (body)
SELECT 'multi second doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs2_idx') IS NOT NULL
       AS multi_spill2;
COMMIT;
SELECT idx::text, count(*) AS log_rows
FROM compaction_log
GROUP BY idx
ORDER BY idx::text;

-- Missing request function warns once and writer data still commits.
TRUNCATE compaction_log;
SET pg_textsearch.compaction_request_function = 'missing_compaction_fn';
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'missing function doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS missing_function_spill;
COMMIT;
SELECT count(*) AS missing_function_log_rows FROM compaction_log;
SELECT count(*) >= 100 AS missing_function_committed
FROM compaction_request_docs;

-- Empty request function warns once and writer data still commits.
TRUNCATE compaction_log;
SET pg_textsearch.compaction_request_function = '';
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'empty function doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS empty_function_spill;
COMMIT;
SELECT count(*) AS empty_function_log_rows FROM compaction_log;
SELECT count(*) >= 120 AS empty_function_committed
FROM compaction_request_docs;

-- Inline mode does not enqueue background requests.
TRUNCATE compaction_log;
SET pg_textsearch.compaction_request_function = 'log_compaction';
SET pg_textsearch.compaction_mode = 'inline';
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'inline doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS inline_spill;
COMMIT;
SELECT count(*) AS inline_log_rows FROM compaction_log;

-- A raising request function is isolated; writer data still commits.
SET pg_textsearch.compaction_mode = 'background';
SET pg_textsearch.compaction_request_function = 'fail_compaction';
BEGIN;
INSERT INTO compaction_request_docs (body)
SELECT 'raising function doc ' || i || ' ' || repeat('filler ', 4)
FROM generate_series(1, 20) i;
SELECT bm25_spill_index('compaction_request_docs_idx') IS NOT NULL
       AS raising_function_spill;
COMMIT;
SELECT count(*) >= 160 AS raising_function_committed
FROM compaction_request_docs;

RESET pg_textsearch.compaction_mode;
RESET pg_textsearch.compaction_request_function;
DROP TABLE compaction_request_docs2 CASCADE;
DROP TABLE compaction_request_docs CASCADE;
DROP TABLE compaction_log;
DROP EXTENSION pg_textsearch CASCADE;
