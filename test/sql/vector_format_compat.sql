-- Test backward compatibility: tpvector_recv must accept legacy v1
-- binary input (pre-1.2 format with int32 freq/lex_len fields and
-- MAXALIGN-padded entries) and transparently convert to v2.
--
-- The generator script (test/scripts/gen_v1_bm25vector.py) emits a
-- COPY BINARY frame containing a single row with a hand-encoded v1
-- bm25vector value. We import via \copy ... FROM PROGRAM, which
-- runs through tpvector_recv → tpvector_v1_to_v2. The resulting
-- (now v2) value is verified by string comparison and equality
-- against a directly-constructed reference.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

CREATE TABLE compat_docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO compat_docs (content) VALUES ('seed document for index');
CREATE INDEX compat_idx ON compat_docs USING bm25(content)
    WITH (text_config='english');

CREATE TABLE compat_import (id int, v bm25vector);

\copy compat_import FROM PROGRAM 'python3 test/scripts/gen_v1_bm25vector.py' WITH (FORMAT binary)

-- Verify the v1 input was decoded correctly.
SELECT id, v::text AS round_tripped FROM compat_import ORDER BY id;

-- Equality against a v2 reference value.
SELECT v = 'compat_idx:{alpha:1,bravo:2}'::bm25vector AS matches_reference
    FROM compat_import;

-- Send round-trip: emit v2 wire bytes, reimport. Confirms
-- end-to-end integrity through the v2 send path.
\copy compat_import TO '/tmp/compat_resend.bin' WITH (FORMAT binary)
CREATE TABLE compat_resend (id int, v bm25vector);
\copy compat_resend FROM '/tmp/compat_resend.bin' WITH (FORMAT binary)
SELECT v::text AS after_v2_resend FROM compat_resend;

-- Cleanup
\! rm -f /tmp/compat_resend.bin
DROP TABLE compat_import;
DROP TABLE compat_resend;
DROP TABLE compat_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
