-- The extension need not be installed in public or first in search_path.
\pset format unaligned
CREATE SCHEMA security_real;
CREATE EXTENSION pg_textsearch SCHEMA security_real;
SET search_path = security_real, public;
CREATE TABLE squat_docs (id integer, content text, tags text[]);
INSERT INTO squat_docs VALUES
    (1, 'target', ARRAY['target']),
    (2, 'other', ARRAY['other']);
CREATE INDEX squat_text_idx ON squat_docs USING bm25(content)
    WITH (text_config='english');
CREATE INDEX squat_array_idx ON squat_docs USING bm25(tags)
    WITH (text_config='english');
SET enable_seqscan = off;

CREATE SCHEMA security_evil;
CREATE TYPE security_evil.bm25query AS (dummy integer);
CREATE FUNCTION security_evil.score(text, text) RETURNS float8
    LANGUAGE plpgsql AS $$ BEGIN RETURN 42; END $$;
CREATE FUNCTION security_evil.score(text, security_real.bm25query) RETURNS float8
    LANGUAGE plpgsql AS $$ BEGIN RETURN 43; END $$;
CREATE FUNCTION security_evil.score(text[], text) RETURNS float8
    LANGUAGE plpgsql AS $$ BEGIN RETURN 44; END $$;
CREATE FUNCTION security_evil.score(text[], security_real.bm25query) RETURNS float8
    LANGUAGE plpgsql AS $$ BEGIN RETURN 45; END $$;
CREATE OPERATOR security_evil.<@> (
    LEFTARG = text, RIGHTARG = text, FUNCTION = security_evil.score);
CREATE OPERATOR security_evil.<@> (
    LEFTARG = text, RIGHTARG = security_real.bm25query,
    FUNCTION = security_evil.score);
CREATE OPERATOR security_evil.<@> (
    LEFTARG = text[], RIGHTARG = text, FUNCTION = security_evil.score);
CREATE OPERATOR security_evil.<@> (
    LEFTARG = text[], RIGHTARG = security_real.bm25query,
    FUNCTION = security_evil.score);
SET search_path = security_evil, security_real, public;
-- Invalidate even an already-warm OID cache under the hostile search_path.
CREATE TYPE security_evil.cache_reset AS (dummy integer);

-- Real operators must still be recognized/re-written for all four signatures.
EXPLAIN (COSTS OFF)
SELECT id FROM squat_docs
ORDER BY content OPERATOR(security_real.<@>) 'target'::text LIMIT 1;
SELECT id FROM squat_docs
ORDER BY content OPERATOR(security_real.<@>) 'target'::text LIMIT 1;
EXPLAIN (COSTS OFF)
SELECT id FROM squat_docs
ORDER BY content OPERATOR(security_real.<@>) 'target'::security_real.bm25query LIMIT 1;
SELECT id FROM squat_docs
ORDER BY content OPERATOR(security_real.<@>) 'target'::security_real.bm25query LIMIT 1;
EXPLAIN (COSTS OFF)
SELECT id FROM squat_docs
ORDER BY tags OPERATOR(security_real.<@>) 'target'::text LIMIT 1;
SELECT id FROM squat_docs
ORDER BY tags OPERATOR(security_real.<@>) 'target'::text LIMIT 1;
EXPLAIN (COSTS OFF)
SELECT id FROM squat_docs
ORDER BY tags OPERATOR(security_real.<@>) 'target'::security_real.bm25query LIMIT 1;
SELECT id FROM squat_docs
ORDER BY tags OPERATOR(security_real.<@>) 'target'::security_real.bm25query LIMIT 1;

-- Parser-selected shadow operators are legitimate user operators, not BM25.
-- Constant left operands must not trigger the extension's column-only check.
SELECT 'x' <@> 'target'::text AS shadow_text,
       'x' <@> 'target'::security_real.bm25query AS shadow_query,
       ARRAY['x'] <@> 'target'::text AS shadow_array_text,
       ARRAY['x'] <@> 'target'::security_real.bm25query AS shadow_array_query;
SELECT content OPERATOR(security_evil.<@>) 'target'::text AS shadow_text,
       content OPERATOR(security_evil.<@>) 'target'::security_real.bm25query AS shadow_query,
       tags OPERATOR(security_evil.<@>) 'target'::text AS shadow_array_text,
       tags OPERATOR(security_evil.<@>) 'target'::security_real.bm25query AS shadow_array_query
FROM squat_docs ORDER BY id;

-- Namespace renames must not leave stale schema names in the OID cache.
ALTER SCHEMA security_real RENAME TO security_renamed;
SET search_path = security_evil, security_renamed, public;
SELECT id FROM security_renamed.squat_docs
ORDER BY content OPERATOR(security_renamed.<@>) 'target'::text LIMIT 1;
RESET search_path;
RESET enable_seqscan;
DROP INDEX security_renamed.squat_text_idx;
DROP INDEX security_renamed.squat_array_idx;
SET client_min_messages = warning;
DROP SCHEMA security_evil CASCADE;
DROP EXTENSION pg_textsearch;

-- Warm the absent-extension cache, then recreate with new object OIDs.
-- This must also work on PG17 releases without an extension syscache.
SELECT 1 AS without_extension;
CREATE EXTENSION pg_textsearch SCHEMA security_renamed;
SET search_path = security_renamed, public;
CREATE INDEX squat_text_idx ON squat_docs USING bm25(content)
    WITH (text_config='english');
SET enable_seqscan = off;
SELECT id FROM squat_docs ORDER BY content <@> 'target'::text LIMIT 1;
SELECT id FROM squat_docs ORDER BY content <@> 'target'::bm25query LIMIT 1;
RESET enable_seqscan;
RESET search_path;
DROP TABLE security_renamed.squat_docs;
DROP EXTENSION pg_textsearch;
DROP SCHEMA security_renamed;
RESET client_min_messages;
\pset format aligned
