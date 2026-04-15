--
-- Test subtransaction (SAVEPOINT) abort scenarios
--
-- Regression test for GitHub issue #269: register SubXactCallback for
-- subtransaction abort cleanup.
--
-- Verifies:
-- 1. Registry/shared memory cleanup when CREATE INDEX in a savepoint
--    is rolled back (OAT_DROP does not fire on subtransaction abort).
-- 2. LWLock tracking reset after scan error in a savepoint.
-- 3. Nested savepoint handling.
--

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

-- Set up a persistent table for these tests
CREATE TABLE subxact_test (id serial PRIMARY KEY, body text);
INSERT INTO subxact_test (body) VALUES
    ('the quick brown fox'),
    ('jumped over the lazy dog'),
    ('the rain in spain falls mainly on the plain');

-- =============================================================
-- Scenario 1: CREATE INDEX inside savepoint, then rollback
-- The index registry entry and shared memory must be cleaned up.
-- =============================================================
BEGIN;
SAVEPOINT sp1;
CREATE INDEX subxact_idx ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
-- Index exists within the savepoint
SELECT body <@> 'fox' FROM subxact_test
    ORDER BY body <@> 'fox' LIMIT 1;
ROLLBACK TO SAVEPOINT sp1;
-- After rollback, the index is gone; verify no crash/leak
-- Creating a new index should succeed (registry is clean)
CREATE INDEX subxact_idx ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
SELECT body <@> 'fox' FROM subxact_test
    ORDER BY body <@> 'fox' LIMIT 1;
COMMIT;

-- =============================================================
-- Scenario 2: Scan error inside savepoint, then continue
-- LWLock tracking must be reset on subtransaction abort.
-- =============================================================
BEGIN;
-- Normal scan works
SELECT id FROM subxact_test
    ORDER BY body <@> to_bm25query('quick', 'subxact_idx')
    LIMIT 1;
SAVEPOINT sp2;
-- Force an error during a query involving the BM25 index
SELECT (body <@> to_bm25query('fox', 'subxact_idx'))::int / 0
    FROM subxact_test;
ROLLBACK TO SAVEPOINT sp2;
-- After rollback, scan should still work (lock tracking reset)
SELECT id FROM subxact_test
    ORDER BY body <@> to_bm25query('quick', 'subxact_idx')
    LIMIT 1;
COMMIT;

-- =============================================================
-- Scenario 3: Nested savepoints - inner abort, outer commit
-- =============================================================
BEGIN;
SAVEPOINT outer_sp;
INSERT INTO subxact_test (body) VALUES ('nested savepoint test');
SAVEPOINT inner_sp;
INSERT INTO subxact_test (body) VALUES ('this will be rolled back');
ROLLBACK TO SAVEPOINT inner_sp;
-- Inner insert rolled back but outer insert persists
RELEASE SAVEPOINT outer_sp;
COMMIT;
SELECT body FROM subxact_test
    WHERE body <@> to_bm25query('nested', 'subxact_idx') < 0
    ORDER BY body <@> to_bm25query('nested', 'subxact_idx');

-- =============================================================
-- Scenario 4: CREATE INDEX in savepoint, rollback, then
-- re-create outside savepoint
-- =============================================================
DROP INDEX subxact_idx;
BEGIN;
SAVEPOINT sp_build;
CREATE INDEX subxact_idx2 ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
ROLLBACK TO SAVEPOINT sp_build;
COMMIT;
-- The rolled-back index should not exist
SELECT count(*) FROM pg_class WHERE relname = 'subxact_idx2';
-- Create it properly now
CREATE INDEX subxact_idx2 ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
SELECT body <@> 'fox' FROM subxact_test
    ORDER BY body <@> 'fox' LIMIT 1;

-- =============================================================
-- Scenario 5: INSERT + error in savepoint, then commit
-- Verify memtable state is consistent after subtransaction abort.
-- =============================================================
BEGIN;
INSERT INTO subxact_test (body) VALUES ('before savepoint five');
SAVEPOINT sp5;
INSERT INTO subxact_test (body) VALUES ('inside savepoint five');
SELECT 1/0;
ROLLBACK TO SAVEPOINT sp5;
INSERT INTO subxact_test (body) VALUES ('after rollback five');
COMMIT;
-- Both 'before' and 'after' should be findable, but not 'inside'
SELECT body FROM subxact_test
    WHERE body <@> to_bm25query('five', 'subxact_idx2') < 0
    ORDER BY body <@> to_bm25query('five', 'subxact_idx2');

-- =============================================================
-- Scenario 6: CREATE INDEX + INSERT in same savepoint, then
-- rollback. Exercises runtime-mode cleanup where memtable hash
-- tables (string_hash, doc_lengths) have been populated in the
-- global DSA and must be destroyed to avoid DSA memory leaks.
-- =============================================================
DROP INDEX subxact_idx2;
BEGIN;
SAVEPOINT sp6;
CREATE INDEX subxact_idx6 ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
-- Insert after CREATE INDEX populates global DSA hash tables
INSERT INTO subxact_test (body) VALUES ('hash table cleanup test');
SELECT body <@> 'cleanup' FROM subxact_test
    ORDER BY body <@> 'cleanup' LIMIT 1;
ROLLBACK TO SAVEPOINT sp6;
COMMIT;
-- Verify the index is gone and registry is clean
SELECT count(*) FROM pg_class WHERE relname = 'subxact_idx6';

-- =============================================================
-- Scenario 7: RELEASE SAVEPOINT (commit sub), then full ROLLBACK
-- Tests promotion: state created in inner subxact is promoted to
-- parent via SUBXACT_EVENT_COMMIT_SUB, then the top-level abort
-- cleans it up via the XactCallback.
-- =============================================================
BEGIN;
SAVEPOINT sp7;
CREATE INDEX subxact_idx7 ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
INSERT INTO subxact_test (body) VALUES ('promotion test row');
RELEASE SAVEPOINT sp7;
-- Index state has been promoted to the top-level transaction.
-- A full ROLLBACK should clean everything up.
ROLLBACK;
SELECT count(*) FROM pg_class WHERE relname = 'subxact_idx7';

-- =============================================================
-- Scenario 8: Repeated savepoint create/rollback cycles
-- Stress the registry cleanup by doing it multiple times to
-- ensure no accumulation of leaked entries.
-- =============================================================
BEGIN;
SAVEPOINT cyc;
CREATE INDEX subxact_cyc ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
ROLLBACK TO SAVEPOINT cyc;

SAVEPOINT cyc;
CREATE INDEX subxact_cyc ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
ROLLBACK TO SAVEPOINT cyc;

SAVEPOINT cyc;
CREATE INDEX subxact_cyc ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
ROLLBACK TO SAVEPOINT cyc;
COMMIT;
SELECT count(*) FROM pg_class WHERE relname = 'subxact_cyc';

-- =============================================================
-- Scenario 9: Deeply nested savepoints (3 levels)
-- Tests that the promotion chain works through multiple levels
-- and that cleanup propagates correctly.
-- =============================================================
BEGIN;
SAVEPOINT lv1;
  SAVEPOINT lv2;
    SAVEPOINT lv3;
    CREATE INDEX subxact_deep ON subxact_test
        USING bm25 (body) WITH (text_config = 'english');
    RELEASE SAVEPOINT lv3;  -- promote to lv2
  RELEASE SAVEPOINT lv2;    -- promote to lv1
ROLLBACK TO SAVEPOINT lv1;  -- abort lv1 => should clean up
COMMIT;
SELECT count(*) FROM pg_class WHERE relname = 'subxact_deep';
-- Re-create should succeed (registry is clean)
CREATE INDEX subxact_deep ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
SELECT body <@> 'fox' FROM subxact_test
    ORDER BY body <@> 'fox' LIMIT 1;
DROP INDEX subxact_deep;

-- =============================================================
-- Scenario 10: Multiple indexes in the same savepoint
-- Tests cleanup of multiple entries at once.
-- =============================================================
CREATE TABLE subxact_test2 (id serial, body text);
INSERT INTO subxact_test2 (body) VALUES ('second table data');
BEGIN;
SAVEPOINT sp10;
CREATE INDEX subxact_m1 ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
CREATE INDEX subxact_m2 ON subxact_test2
    USING bm25 (body) WITH (text_config = 'english');
ROLLBACK TO SAVEPOINT sp10;
COMMIT;
SELECT count(*) FROM pg_class
    WHERE relname IN ('subxact_m1', 'subxact_m2');
-- Both should be re-creatable
CREATE INDEX subxact_m1 ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
CREATE INDEX subxact_m2 ON subxact_test2
    USING bm25 (body) WITH (text_config = 'english');
SELECT body <@> 'fox' FROM subxact_test
    ORDER BY body <@> 'fox' LIMIT 1;
SELECT body <@> 'data' FROM subxact_test2
    ORDER BY body <@> 'data' LIMIT 1;

-- =============================================================
-- Scenario 11: Multiple scan errors across savepoints
-- Tests repeated lock tracking resets within one transaction.
-- =============================================================
BEGIN;
SELECT id FROM subxact_test
    ORDER BY body <@> to_bm25query('fox', 'subxact_m1')
    LIMIT 1;
SAVEPOINT e1;
SELECT 1/0;
ROLLBACK TO SAVEPOINT e1;
-- Scan works after first error
SELECT id FROM subxact_test
    ORDER BY body <@> to_bm25query('fox', 'subxact_m1')
    LIMIT 1;
SAVEPOINT e2;
SELECT 1/0;
ROLLBACK TO SAVEPOINT e2;
-- Scan works after second error
SELECT id FROM subxact_test
    ORDER BY body <@> to_bm25query('fox', 'subxact_m1')
    LIMIT 1;
SAVEPOINT e3;
SELECT 1/0;
ROLLBACK TO SAVEPOINT e3;
-- Scan works after third error
SELECT id FROM subxact_test
    ORDER BY body <@> to_bm25query('fox', 'subxact_m1')
    LIMIT 1;
COMMIT;

-- =============================================================
-- Scenario 12: Savepoint rollback does NOT affect pre-existing
-- index. Verifies that InvalidSubTransactionId entries survive.
-- =============================================================
BEGIN;
-- Scan pre-existing index (caches local state with InvalidSubTxnId)
SELECT id FROM subxact_test
    ORDER BY body <@> to_bm25query('quick', 'subxact_m1')
    LIMIT 1;
SAVEPOINT sp12;
INSERT INTO subxact_test (body) VALUES ('will be rolled back');
SELECT 1/0;
ROLLBACK TO SAVEPOINT sp12;
-- Pre-existing index should still work normally
INSERT INTO subxact_test (body) VALUES ('survives rollback');
COMMIT;
SELECT body FROM subxact_test
    WHERE body <@> to_bm25query('survives', 'subxact_m1') < 0;

-- =============================================================
-- Scenario 13: bm25_summarize_index on a re-created index after
-- savepoint rollback. Direct verification that the index state
-- is fully functional.
-- =============================================================
DROP INDEX subxact_m1;
BEGIN;
SAVEPOINT sp13;
CREATE INDEX subxact_verify ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
INSERT INTO subxact_test (body) VALUES ('verify will vanish');
ROLLBACK TO SAVEPOINT sp13;
COMMIT;
-- Create properly and verify internal state is consistent
CREATE INDEX subxact_verify ON subxact_test
    USING bm25 (body) WITH (text_config = 'english');
SELECT bm25_summarize_index('subxact_verify') IS NOT NULL
    AS summary_ok;
SELECT body <@> 'fox' FROM subxact_test
    ORDER BY body <@> 'fox' LIMIT 1;

-- Cleanup
DROP TABLE subxact_test2;
DROP TABLE subxact_test;
DROP EXTENSION pg_textsearch CASCADE;
