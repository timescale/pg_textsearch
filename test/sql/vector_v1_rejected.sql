-- Test that legacy v1 bm25vector binary input is detected and
-- rejected with a clear error (rather than silently misinterpreted
-- as v2 and producing garbage results or crashes).
--
-- pg_textsearch < 1.2.0 used a different on-disk bm25vector layout.
-- bm25vector values are almost always transient (built by
-- to_bm25vector for a query, consumed during scan) — actually
-- persisting them in user tables was uncommon — so 1.2.0 dropped
-- v1 read support. Anyone with persisted v1 data must recompute
-- via to_bm25vector(text, index_name) from the source text.
--
-- This test pins the rejection behavior: the error message must
-- direct users to the correct recovery path.
--
-- Also covers the varint multi-byte encode/decode paths in v2 at
-- byte-width boundaries (1/2/3/4-byte freq, 1/2-byte lex_len) —
-- these aren't exercised by typical tokenized text where almost
-- all frequencies are 1.

CREATE EXTENSION IF NOT EXISTS pg_textsearch;

CREATE TABLE compat_docs (id SERIAL PRIMARY KEY, content TEXT);
INSERT INTO compat_docs (content) VALUES ('seed document for index');
CREATE INDEX compat_idx ON compat_docs USING bm25(content)
    WITH (text_config='english');

-- ========================================================================
-- v1 input rejection
-- ========================================================================

CREATE TABLE v1_reject (id int, v bm25vector);

-- Should fail with a clear error pointing at the version mismatch.
\set VERBOSITY terse
\copy v1_reject FROM PROGRAM 'python3 test/scripts/gen_v1_bm25vector.py' WITH (FORMAT binary)
\set VERBOSITY default

-- Confirm nothing was inserted.
SELECT count(*) AS rows_after_rejection FROM v1_reject;

-- ========================================================================
-- Varint encoding boundary coverage
--
-- The v2 per-entry header uses LEB128 varint for both `frequency`
-- and `lexeme_len`. The 1-byte path covers values 0..127; values
-- ≥ 128 hit the multi-byte encode/decode path. Almost all
-- naturally-occurring frequencies are 1 (POSDATALEN of a tsvector
-- word), so without explicit fixtures the multi-byte varint path
-- isn't exercised. These pin both the encode side (tpvector_in →
-- create_tpvector_from_strings → tpvector_varint_encode) and the
-- decode side (tpvector_out → tpvector_entry_decode →
-- tpvector_varint_decode) at every byte-width transition.
-- ========================================================================

-- Single-entry boundaries
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
-- 3-byte freq encodings.
SELECT 'compat_idx:{a:1,b:127,c:128,d:16383,e:16384}'::bm25vector::text
    AS mixed_freq_widths;

-- Equality through varint boundaries.
WITH v AS (
    SELECT 'compat_idx:{x:50000,y:1}'::bm25vector AS orig
)
SELECT orig::text AS rendered,
       orig = orig::text::bm25vector AS round_trip_eq
    FROM v;

-- Long-lexeme path (multi-byte lex_len varint).
WITH s AS (
    SELECT 'compat_idx:{' || repeat('a', 200) || ':1}' AS literal
)
SELECT length(literal)               AS literal_bytes,
       literal::bm25vector::text = literal AS roundtrip_200char_lex
    FROM s;

-- Cleanup
DROP TABLE v1_reject;
DROP TABLE compat_docs CASCADE;
DROP EXTENSION pg_textsearch CASCADE;
