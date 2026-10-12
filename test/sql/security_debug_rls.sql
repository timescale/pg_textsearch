-- A non-owner can request DEBUG1 messages, but not pre-RLS CTIDs/scores.
\pset format unaligned
-- Keep DEBUG1 assertions independent of the server's statement logging.
SET log_statement = none;
CREATE EXTENSION pg_textsearch;
CREATE ROLE debug_rls_reader;
CREATE TABLE debug_rls (id integer, content text);
INSERT INTO debug_rls VALUES
    (1, 'target target target'),
    (2, 'target filler filler filler'),
    (3, 'other');
CREATE INDEX debug_rls_idx ON debug_rls USING bm25(content)
    WITH (text_config='english');
ALTER TABLE debug_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY visible_row ON debug_rls TO debug_rls_reader USING (id = 2);
GRANT SELECT ON debug_rls TO debug_rls_reader;
SET enable_seqscan = off;
SET pg_textsearch.log_scores = off;
SET ROLE debug_rls_reader;
EXPLAIN (COSTS OFF)
SELECT id FROM debug_rls ORDER BY content <@> 'target' LIMIT 1;
SET client_min_messages = debug1;
SELECT id FROM debug_rls ORDER BY content <@> 'target' LIMIT 1;
-- Neither enabling score logging nor relaxing the RLS guard is permitted.
SET pg_textsearch.log_scores = on;
SET pg_textsearch.allow_rls = off;
RESET client_min_messages;
RESET ROLE;

-- Administrators can explicitly opt in to score diagnostics.
SET pg_textsearch.log_scores = on;
SELECT id FROM debug_rls ORDER BY content <@> 'target' LIMIT 2;
SET pg_textsearch.log_scores = off;
SET ROLE debug_rls_reader;
SET client_min_messages = debug1;
SELECT id FROM debug_rls ORDER BY content <@> 'target' LIMIT 1;
RESET client_min_messages;
RESET ROLE;
RESET enable_seqscan;
RESET pg_textsearch.log_scores;
DROP TABLE debug_rls;
DROP ROLE debug_rls_reader;
DROP EXTENSION pg_textsearch;
RESET log_statement;
\pset format aligned
