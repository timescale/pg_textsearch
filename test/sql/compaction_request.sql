CREATE EXTENSION IF NOT EXISTS pg_textsearch;

\pset format unaligned
SHOW pg_textsearch.compaction_mode;
SET client_min_messages = warning;
SET pg_textsearch.segments_per_level = 2;
SET pg_textsearch.compaction_mode = 'background';

CREATE SEQUENCE compaction_request_calls;
SELECT setval('compaction_request_calls', 1, true) AS calls_before \gset
CREATE TABLE compaction_callback_rows (idx regclass);

CREATE FUNCTION record_compaction_request(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM nextval('compaction_request_calls');
    INSERT INTO compaction_callback_rows VALUES (idx);
END;
$$;

CREATE FUNCTION fail_compaction_request(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'intentional request failure for %', idx;
END;
$$;

CREATE FUNCTION cancel_compaction_request(idx regclass) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_catalog.pg_cancel_backend(pg_catalog.pg_backend_pid());
    PERFORM pg_catalog.pg_sleep(1);
END;
$$;

SET pg_textsearch.compaction_request_function =
    'record_compaction_request';

-- The callback GUC validates syntax only.
SET pg_textsearch.compaction_request_function = 'missing_callback';
SET pg_textsearch.compaction_request_function =
    'public.record_compaction_request';
SET pg_textsearch.compaction_request_function = 'invalid..callback';
SET pg_textsearch.compaction_request_function = 'db.schema.callback';
SET pg_textsearch.compaction_request_function = '""';
SET pg_textsearch.compaction_request_function = 'schema.""';
SET pg_textsearch.compaction_request_function = '"".callback';
SET pg_textsearch.compaction_request_function =
    'record_compaction_request';

CREATE TABLE request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX request_docs_idx ON request_docs
    USING bm25(body) WITH (text_config = 'english');
CREATE TABLE request_docs2 (id serial PRIMARY KEY, body text);
CREATE INDEX request_docs2_idx ON request_docs2
    USING bm25(body) WITH (text_config = 'english');

-- Seed below the threshold.
INSERT INTO request_docs (body)
SELECT 'seed one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS seed_one;
INSERT INTO request_docs2 (body)
SELECT 'seed two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs2_idx') IS NOT NULL AS seed_two;

-- Top-level abort discards the pending request.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'abort ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS abort_spill;
ROLLBACK;
SELECT last_value - :calls_before AS abort_requests
FROM compaction_request_calls;

-- Commit dispatches once and callback-local writes are rolled back.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'commit ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS commit_spill;
COMMIT;
SELECT last_value - :calls_before AS commit_requests
FROM compaction_request_calls;
SELECT count(*) AS callback_local_rows FROM compaction_callback_rows;

-- Repeated requests for one index are deduplicated.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'dedup one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS dedup_one;
INSERT INTO request_docs (body)
SELECT 'dedup two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS dedup_two;
COMMIT;
SELECT last_value - :calls_before AS deduplicated_requests
FROM compaction_request_calls;

-- Different indexes each dispatch once.
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'multi one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL AS multi_one;
INSERT INTO request_docs2 (body)
SELECT 'multi two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs2_idx') IS NOT NULL AS multi_two;
COMMIT;
SELECT last_value - :calls_before AS multiple_index_requests
FROM compaction_request_calls;

-- A savepoint rollback retains physical compaction debt and its request.
CREATE TABLE savepoint_docs (id serial PRIMARY KEY, body text);
CREATE INDEX savepoint_docs_idx ON savepoint_docs
    USING bm25(body) WITH (text_config = 'english');
INSERT INTO savepoint_docs (body)
SELECT 'savepoint seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('savepoint_docs_idx') IS NOT NULL;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
SAVEPOINT request_savepoint;
INSERT INTO savepoint_docs (body)
SELECT 'savepoint rollback ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('savepoint_docs_idx') IS NOT NULL;
ROLLBACK TO request_savepoint;
COMMIT;
SELECT last_value - :calls_before AS savepoint_requests
FROM compaction_request_calls;

-- Rolling back a DROP restores an already-pending request.
CREATE TABLE rollback_drop_docs (id serial PRIMARY KEY, body text);
CREATE INDEX rollback_drop_docs_idx ON rollback_drop_docs
    USING bm25(body) WITH (text_config = 'english');
INSERT INTO rollback_drop_docs (body)
SELECT 'rollback drop seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('rollback_drop_docs_idx') IS NOT NULL;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO rollback_drop_docs (body)
SELECT 'rollback drop pending ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('rollback_drop_docs_idx') IS NOT NULL;
SAVEPOINT drop_request;
DROP INDEX rollback_drop_docs_idx;
ROLLBACK TO drop_request;
COMMIT;
SELECT last_value - :calls_before AS rollback_drop_requests
FROM compaction_request_calls;

-- An index and request created in an aborted savepoint are not dispatched.
CREATE TABLE aborted_index_docs (id serial PRIMARY KEY, body text);
SET pg_textsearch.compaction_request_function =
    'missing_stale_callback';
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
SAVEPOINT create_request;
CREATE INDEX aborted_index_docs_idx ON aborted_index_docs
    USING bm25(body) WITH (text_config = 'english');
INSERT INTO aborted_index_docs (body)
SELECT 'aborted index one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('aborted_index_docs_idx') IS NOT NULL;
INSERT INTO aborted_index_docs (body)
SELECT 'aborted index two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('aborted_index_docs_idx') IS NOT NULL;
ROLLBACK TO create_request;
COMMIT;
SELECT last_value - :calls_before AS aborted_index_requests
FROM compaction_request_calls;
SELECT to_regclass('aborted_index_docs_idx') IS NULL
       AS aborted_index_absent;
SET pg_textsearch.compaction_request_function =
    'record_compaction_request';

-- A dropped index is removed from the pending set.
CREATE TABLE dropped_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX dropped_request_docs_idx ON dropped_request_docs
    USING bm25(body) WITH (text_config = 'english');
INSERT INTO dropped_request_docs (body)
SELECT 'drop seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('dropped_request_docs_idx') IS NOT NULL;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO dropped_request_docs (body)
SELECT 'drop pending ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('dropped_request_docs_idx') IS NOT NULL;
DROP INDEX dropped_request_docs_idx;
COMMIT;
SELECT last_value - :calls_before AS dropped_index_requests
FROM compaction_request_calls;

-- A missing callback still warns when at least one live request exists.
CREATE FUNCTION disappearing_request(regclass) RETURNS void
LANGUAGE sql AS $$ SELECT NULL::void $$;
SET pg_textsearch.compaction_request_function = 'disappearing_request';
DROP FUNCTION disappearing_request(regclass);
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO request_docs (body)
SELECT 'disappearing callback ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL;
COMMIT;
SELECT last_value - :calls_before AS disappeared_callback_requests
FROM compaction_request_calls;
SELECT count(*) = 20 AS disappeared_callback_committed
FROM request_docs WHERE body LIKE 'disappearing callback %';

-- Ordinary callback errors warn and preserve the writer transaction.
SET pg_textsearch.compaction_request_function = 'fail_compaction_request';
BEGIN;
INSERT INTO request_docs (body)
SELECT 'callback failure ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('request_docs_idx') IS NOT NULL;
COMMIT;
SELECT count(*) = 20 AS callback_failure_committed
FROM request_docs WHERE body LIKE 'callback failure %';

-- PRE_PREPARE dispatches and PREPARE leaves no stale backend request.
CREATE TABLE prepared_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX prepared_request_docs_idx ON prepared_request_docs
    USING bm25(body) WITH (text_config = 'english');
SET pg_textsearch.compaction_request_function =
    'record_compaction_request';
INSERT INTO prepared_request_docs (body)
SELECT 'prepare seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('prepared_request_docs_idx') IS NOT NULL;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
INSERT INTO prepared_request_docs (body)
SELECT 'prepare pending ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('prepared_request_docs_idx') IS NOT NULL;
PREPARE TRANSACTION 'compaction_request_prepare';
SELECT last_value - :calls_before AS prepare_requests
FROM compaction_request_calls;
SELECT last_value AS calls_before FROM compaction_request_calls \gset
BEGIN;
SELECT 1 AS backend_reused;
COMMIT;
SELECT last_value - :calls_before AS stale_requests
FROM compaction_request_calls;
ROLLBACK PREPARED 'compaction_request_prepare';

-- Temporary indexes compact inline in background mode.
CREATE TEMP TABLE temp_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX temp_request_docs_idx ON temp_request_docs
    USING bm25(body) WITH (text_config = 'english');
SELECT last_value AS calls_before FROM compaction_request_calls \gset
INSERT INTO temp_request_docs (body)
SELECT 'temp one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('temp_request_docs_idx') IS NOT NULL;
INSERT INTO temp_request_docs (body)
SELECT 'temp two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('temp_request_docs_idx') IS NOT NULL;
SELECT last_value - :calls_before AS temp_requests
FROM compaction_request_calls;
SELECT NOT bm25_needs_compaction('temp_request_docs_idx'::regclass)
       AS temp_compacted_inline;

-- A final serial build batch must receive the same inline compaction policy
-- as budget-triggered intermediate batches.
SET max_parallel_maintenance_workers = 0;
SET maintenance_work_mem = '1MB';
CREATE TABLE final_build_docs (
    id integer PRIMARY KEY,
    body text
);
INSERT INTO final_build_docs
SELECT i, 'token' || i || ' ' || repeat(md5(i::text) || ' ', 8)
FROM generate_series(1, 10000) i;
CREATE INDEX final_build_docs_idx ON final_build_docs
    USING bm25(body) WITH (text_config = 'simple');
SELECT bm25_level_counts('final_build_docs_idx'::regclass) =
           ARRAY[0, 1, 0, 0, 0, 0, 0, 0]
       AS final_build_batch_compacted;

-- Off mode leaves debt in place and dispatches nothing.
CREATE TABLE off_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX off_request_docs_idx ON off_request_docs
    USING bm25(body) WITH (text_config = 'english');
SET pg_textsearch.compaction_mode = 'off';
SELECT last_value AS calls_before FROM compaction_request_calls \gset
INSERT INTO off_request_docs (body)
SELECT 'off one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('off_request_docs_idx') IS NOT NULL;
INSERT INTO off_request_docs (body)
SELECT 'off two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('off_request_docs_idx') IS NOT NULL;
SELECT last_value - :calls_before AS off_requests
FROM compaction_request_calls;
SELECT bm25_needs_compaction('off_request_docs_idx'::regclass)
       AS off_debt_remains;

-- Inline mode compacts synchronously and dispatches nothing.
CREATE TABLE inline_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX inline_request_docs_idx ON inline_request_docs
    USING bm25(body) WITH (text_config = 'english');
SET pg_textsearch.compaction_mode = 'inline';
SELECT last_value AS calls_before FROM compaction_request_calls \gset
INSERT INTO inline_request_docs (body)
SELECT 'inline one ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('inline_request_docs_idx') IS NOT NULL;
INSERT INTO inline_request_docs (body)
SELECT 'inline two ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('inline_request_docs_idx') IS NOT NULL;
SELECT last_value - :calls_before AS inline_requests
FROM compaction_request_calls;
SELECT NOT bm25_needs_compaction('inline_request_docs_idx'::regclass)
       AS inline_compacted;

-- Cancellation remains an error and aborts the writer transaction.
CREATE TABLE canceled_request_docs (id serial PRIMARY KEY, body text);
CREATE INDEX canceled_request_docs_idx ON canceled_request_docs
    USING bm25(body) WITH (text_config = 'english');
SET pg_textsearch.compaction_mode = 'background';
SET pg_textsearch.compaction_request_function =
    'cancel_compaction_request';
INSERT INTO canceled_request_docs (body)
SELECT 'cancel seed ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('canceled_request_docs_idx') IS NOT NULL;
BEGIN;
INSERT INTO canceled_request_docs (body)
SELECT 'cancel pending ' || i FROM generate_series(1, 20) i;
SELECT bm25_spill_index('canceled_request_docs_idx') IS NOT NULL;
\set VERBOSITY terse
COMMIT;
\set VERBOSITY default
SELECT count(*) AS canceled_rows
FROM canceled_request_docs WHERE body LIKE 'cancel pending %';

RESET pg_textsearch.compaction_mode;
RESET pg_textsearch.compaction_request_function;
DROP TABLE canceled_request_docs CASCADE;
DROP TABLE inline_request_docs CASCADE;
DROP TABLE off_request_docs CASCADE;
DROP TABLE final_build_docs CASCADE;
DROP TABLE prepared_request_docs CASCADE;
DROP TABLE dropped_request_docs CASCADE;
DROP TABLE aborted_index_docs CASCADE;
DROP TABLE rollback_drop_docs CASCADE;
DROP TABLE savepoint_docs CASCADE;
DROP TABLE request_docs2 CASCADE;
DROP TABLE request_docs CASCADE;
DROP TABLE compaction_callback_rows;
DROP SEQUENCE compaction_request_calls CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
