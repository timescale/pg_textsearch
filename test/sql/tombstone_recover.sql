-- Self-healing recovery of a corrupt deferred-free tombstone chain
-- (issue #427).
--
-- Prevention (the allocator refusing to reuse a live page) stops NEW
-- corruption, but an index already corrupted by an older binary stays
-- wedged: every write path triggers a merge/vacuum tombstone drain that
-- raises "corrupt tombstone page ..." and aborts.  The drain must
-- instead recover — drop the unverifiable tail of the chain, warn, and
-- let writes proceed — so operators are not forced to REINDEX.
CREATE EXTENSION IF NOT EXISTS pg_textsearch;
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 0;
-- Keep the recovery WARNING out of the golden output (it names a
-- block number); the test asserts the post-recovery state instead.
SET client_min_messages = error;

CREATE TABLE recover_docs (id int, body text)
    WITH (autovacuum_enabled = false);
INSERT INTO recover_docs
SELECT g, 'alpha beta gamma delta term' || (g % 50)
FROM generate_series(1, 2000) g;
CREATE INDEX recover_idx ON recover_docs
    USING bm25 (body) WITH (text_config = 'english');

INSERT INTO recover_docs
SELECT g, 'alpha beta term' || (g % 50)
FROM generate_series(2001, 4000) g;
SELECT bm25_spill_index('recover_idx') > 0 AS spilled;
SELECT bm25_force_merge('recover_idx');
SELECT bm25_pending_free_pages('recover_idx') > 0 AS parked;

-- Simulate an already-corrupt chain node, as an older page-reuse bug
-- would have left behind.
SELECT bm25_test_corrupt_tombstone_head('recover_idx') > 0 AS corrupted;

-- A VACUUM drives the tombstone drain.  It must self-heal (drop the
-- corrupt tail with a WARNING) instead of raising and wedging the
-- index for every future write.
VACUUM recover_docs;

-- The chain was reset past the corruption, so the drain no longer
-- errors and reports a clean (empty) pending set.
SELECT bm25_pending_free_pages('recover_idx') AS pending_after_recovery;

-- Writes and queries proceed normally after recovery.
INSERT INTO recover_docs VALUES (999001, 'alpha beta gamma probe');
SELECT count(*) > 0 AS has_hits
FROM (
    SELECT 1 FROM recover_docs
    ORDER BY body <@> to_bm25query('probe', 'recover_idx')
    LIMIT 10
) s;

DROP TABLE recover_docs;
DROP EXTENSION pg_textsearch CASCADE;
