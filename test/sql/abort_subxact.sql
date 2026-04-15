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

-- Cleanup
DROP TABLE subxact_test;
DROP EXTENSION pg_textsearch CASCADE;
