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

-- ========================================================================
-- Varint encoding boundary coverage
--
-- The v2 per-entry header uses LEB128 varint for both `frequency`
-- and `lexeme_len`. The 1-byte path covers values 0..127; values
-- ≥ 128 hit the multi-byte encode/decode path. Almost all
-- naturally-occurring frequencies in tokenized text are 1, so
-- without explicit boundary fixtures the multi-byte varint path
-- isn't exercised.
--
-- These fixtures pin both the encode side (tpvector_in →
-- create_tpvector_from_strings → tpvector_varint_encode) and the
-- decode side (tpvector_out → tpvector_entry_decode →
-- tpvector_varint_decode), at every byte-width transition.
-- Multi-entry vectors also verify get_tpvector_next_entry walks
-- correctly across mixed-width entries.
-- ========================================================================

-- Single-entry boundaries (each value picks a different varint width)
SELECT 'compat_idx:{lex:0}'::bm25vector::text       AS freq_0;       -- 1 byte
SELECT 'compat_idx:{lex:1}'::bm25vector::text       AS freq_1;       -- 1 byte
SELECT 'compat_idx:{lex:127}'::bm25vector::text     AS freq_127;     -- 1 byte (max)
SELECT 'compat_idx:{lex:128}'::bm25vector::text     AS freq_128;     -- 2 bytes (min)
SELECT 'compat_idx:{lex:16383}'::bm25vector::text   AS freq_16383;   -- 2 bytes (max)
SELECT 'compat_idx:{lex:16384}'::bm25vector::text   AS freq_16384;   -- 3 bytes (min)
SELECT 'compat_idx:{lex:2097151}'::bm25vector::text AS freq_2097151; -- 3 bytes (max)
SELECT 'compat_idx:{lex:2097152}'::bm25vector::text AS freq_2097152; -- 4 bytes (min)

-- Multi-entry walk across mixed-width frequencies. Verifies
-- get_tpvector_next_entry correctly advances past 1-, 2-, and
-- 3-byte freq encodings before reading the next entry's freq.
SELECT 'compat_idx:{a:1,b:127,c:128,d:16383,e:16384}'::bm25vector::text
    AS mixed_freq_widths;

-- Equality through varint boundaries: a vector with a high-width
-- freq must compare equal to itself round-tripped via output→input.
WITH v AS (
    SELECT 'compat_idx:{x:50000,y:1}'::bm25vector AS orig
)
SELECT orig::text AS rendered,
       orig = orig::text::bm25vector AS round_trip_eq
    FROM v;

-- Long-lexeme path (uint16 lex_len → 2-byte varint). The strings
-- regression test already exercises >255-byte lexemes via the
-- index-build path, but doing it through the type's text I/O is
-- a tighter unit test.
SELECT length((repeat('a', 200) || ':1')) AS entry_len_bytes,
       'compat_idx:{' || repeat('a', 200) || ':1}'::bm25vector::text
           = 'compat_idx:{' || repeat('a', 200) || ':1}' AS roundtrip_200char_lex;

-- Cleanup
\! rm -f /tmp/compat_resend.bin
DROP TABLE compat_import;
DROP TABLE compat_resend;
DROP TABLE compat_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
