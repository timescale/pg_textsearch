-- Coverage / regression test for the BMW block_max_skip_advance path
-- (issue #355).
--
-- The fix in src/scoring/bmw.c changed block_max_skip_advance() to
-- pick the pivot term whose current block ends *soonest* as the
-- term to seek forward, instead of the highest-max_score term. The
-- old selection could pick a term whose cur_doc_id was already past
-- min_block_end + 1, making seek_term_to_doc a no-op and spinning
-- the outer WAND loop forever on certain MS MARCO data
-- distributions.
--
-- A deterministic SQL workload that triggers the *old* infinite loop
-- has been elusive to construct synthetically: real-world repro
-- requires the ~8.8M-passage MS MARCO corpus with concurrent-insert
-- segment topology. Internal HorizonDB tests at Microsoft are the
-- real validation signal for the bug.
--
-- This test is a coverage-focused smoke test: it builds a workload
-- that reliably enters block_max_skip_advance many times (verified
-- via pg_textsearch.log_bmw_stats: seeks_performed > 0), exercising
-- the patched code path. statement_timeout ensures any future
-- regression that reintroduces a no-op-seek hang is caught quickly
-- rather than burning the whole test run.
CREATE EXTENSION IF NOT EXISTS pg_textsearch;

SET enable_seqscan = off;
SET statement_timeout = '30s';

CREATE TABLE bmw_skip_advance (id serial PRIMARY KEY, content text);

-- Anchor docs: short with high TF on two query terms, raise the
-- top-k threshold so most subsequent blocks fail block_max upper
-- bound and force the WAND loop into block_max_skip_advance.
INSERT INTO bmw_skip_advance (content)
SELECT 'alpha alpha alpha alpha alpha beta beta beta beta beta'
FROM generate_series(1, 20);

-- Bulk: each doc omits exactly one of the four query terms in
-- rotation. The four per-term posting lists end up misaligned, so
-- per-term block boundaries land at different doc_ids -- which is
-- exactly the data condition for the WAND skip-advance path.
INSERT INTO bmw_skip_advance (content)
SELECT CASE i % 4
         WHEN 0 THEN       'beta gamma delta '
         WHEN 1 THEN 'alpha gamma delta '
         WHEN 2 THEN 'alpha beta delta '
         ELSE        'alpha beta gamma '
       END
       || array_to_string(ARRAY(
           SELECT 'filler' || g FROM generate_series(1, 80) g), ' ')
FROM generate_series(1, 5000) i;

CREATE INDEX bmw_skip_advance_idx ON bmw_skip_advance USING bm25(content)
    WITH (text_config = 'simple');

-- This query exercises block_max_skip_advance heavily. Asserting it
-- terminates and returns 10 rows is the regression signal -- any
-- future change that reintroduces the no-op-seek would cause this to
-- hang and statement_timeout would fire.
SELECT count(*) AS got_results FROM (
    SELECT id FROM bmw_skip_advance
    ORDER BY content <@> to_bm25query(
        'alpha beta gamma delta', 'bmw_skip_advance_idx')
    LIMIT 10
) sub;

-- Same query under BMW stats logging, to exercise the
-- seeks_performed / blocks_skipped accounting in the patched
-- function. (The stats LOG line itself is filtered out of pg_regress
-- diff comparison via :v variable; we just want the code path
-- exercised.)
SET pg_textsearch.log_bmw_stats = on;
SELECT count(*) AS got_results FROM (
    SELECT id FROM bmw_skip_advance
    ORDER BY content <@> to_bm25query(
        'alpha beta gamma delta', 'bmw_skip_advance_idx')
    LIMIT 10
) sub;
SET pg_textsearch.log_bmw_stats = off;

-- Now exercise the *memtable* scoring paths (single-term and
-- multi-term), where the CHECK_FOR_INTERRUPTS calls added by this
-- patch sit. Above this point everything was in segments after
-- CREATE INDEX. Inserting fresh docs here without forcing a spill
-- leaves them in the memtable, so the next queries traverse both
-- segments (via BMW skip-advance) and the memtable.
INSERT INTO bmw_skip_advance (content)
SELECT 'alpha beta gamma delta extra_memtable_token'
FROM generate_series(1, 100);

-- Single-term query against memtable (score_memtable_single_term).
SELECT count(*) FROM (
    SELECT id FROM bmw_skip_advance
    ORDER BY content <@> to_bm25query(
        'extra_memtable_token', 'bmw_skip_advance_idx')
    LIMIT 5
) sub;

-- Multi-term query against memtable (score_memtable_multi_term).
SELECT count(*) FROM (
    SELECT id FROM bmw_skip_advance
    ORDER BY content <@> to_bm25query(
        'alpha extra_memtable_token', 'bmw_skip_advance_idx')
    LIMIT 5
) sub;

DROP TABLE bmw_skip_advance;
DROP EXTENSION pg_textsearch CASCADE;
