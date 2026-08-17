-- Standalone BM25 scoring must enforce the invoking role's SELECT
-- privilege on the indexed relation and column. Opening a caller-named
-- index for scoring bypasses the executor's range-table permission
-- checks, so the operator function performs the ACL check itself.

CREATE EXTENSION pg_textsearch;

-- Victim objects live in a schema the attacker cannot use.
CREATE SCHEMA victim;
CREATE TABLE victim.docs (id serial, secret text, note text);
INSERT INTO victim.docs (secret, note) VALUES
    ('alpha secret token', 'hello world'),
    ('beta secret token', 'goodbye world');
CREATE INDEX docs_bm25 ON victim.docs USING bm25(secret)
    WITH (text_config='english');

-- Expression index over two columns not indexed directly.
CREATE TABLE victim.expr_docs (id serial, a text, b text);
INSERT INTO victim.expr_docs (a, b) VALUES ('alpha apple', 'beta banana');
CREATE INDEX expr_bm25 ON victim.expr_docs USING bm25((a || ' ' || b))
    WITH (text_config='english');

-- Partial index whose predicate references a non-indexed column.
CREATE TABLE victim.part_docs (id serial, body text, flag boolean);
INSERT INTO victim.part_docs (body, flag) VALUES
    ('alpha apple', true),
    ('hidden text', false);
CREATE INDEX part_bm25 ON victim.part_docs USING bm25(body)
    WITH (text_config='english') WHERE flag;

-- Idempotent role setup
DO $$
DECLARE
    r text;
BEGIN
    FOREACH r IN ARRAY ARRAY['bm25_attacker', 'bm25_reader',
                             'bm25_colreader', 'bm25_wrongcol',
                             'bm25_nousage', 'bm25_exprreader',
                             'bm25_predreader']
    LOOP
        IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = r) THEN
            EXECUTE format('REASSIGN OWNED BY %I TO CURRENT_USER', r);
            EXECUTE format('DROP OWNED BY %I CASCADE', r);
            EXECUTE format('DROP ROLE %I', r);
        END IF;
    END LOOP;
END $$;

-- Connect-only attacker: no schema usage, no table/column SELECT.
CREATE ROLE bm25_attacker LOGIN;

-- Reader with full table SELECT.
CREATE ROLE bm25_reader LOGIN;
GRANT USAGE ON SCHEMA victim TO bm25_reader;
GRANT SELECT ON victim.docs TO bm25_reader;

-- Reader with column-level SELECT on the indexed column only.
CREATE ROLE bm25_colreader LOGIN;
GRANT USAGE ON SCHEMA victim TO bm25_colreader;
GRANT SELECT (secret) ON victim.docs TO bm25_colreader;

-- Reader with column-level SELECT on a non-indexed column only.
CREATE ROLE bm25_wrongcol LOGIN;
GRANT USAGE ON SCHEMA victim TO bm25_wrongcol;
GRANT SELECT (note) ON victim.docs TO bm25_wrongcol;

-- Reader holding table SELECT but lacking schema USAGE: an index scan
-- would be denied at the schema, so scoring must be too.
CREATE ROLE bm25_nousage LOGIN;
GRANT SELECT ON victim.docs TO bm25_nousage;

-- Reader used to exercise the expression-index column set.
CREATE ROLE bm25_exprreader LOGIN;
GRANT USAGE ON SCHEMA victim TO bm25_exprreader;
GRANT SELECT (a) ON victim.expr_docs TO bm25_exprreader;

-- Reader used to exercise the partial-index predicate column set.
CREATE ROLE bm25_predreader LOGIN;
GRANT USAGE ON SCHEMA victim TO bm25_predreader;
GRANT SELECT (body) ON victim.part_docs TO bm25_predreader;

-- Owner baseline: scoring works.
SELECT ('alpha'::text <@>
        (SELECT to_bm25query('alpha', 'victim.docs_bm25'))) < 0
    AS owner_present;

------------------------------------------------------------------
-- Attacker: every standalone scoring path must be denied.
------------------------------------------------------------------
SET ROLE bm25_attacker;

-- Scalar-subquery constructor (hides the constructor from the planner).
SELECT 'alpha'::text <@>
       (SELECT to_bm25query('alpha', 'victim.docs_bm25')) AS attacker_present;

-- Absent term must be denied too (no zero-vs-negative oracle).
SELECT 'zzzabsent'::text <@>
       (SELECT to_bm25query('zzzabsent', 'victim.docs_bm25')) AS attacker_absent;

-- text[] array scoring path.
SELECT ARRAY['alpha']::text[] <@>
       (SELECT to_bm25query('alpha', 'victim.docs_bm25')) AS attacker_array;

RESET ROLE;

------------------------------------------------------------------
-- Reader with column-level SELECT on a non-indexed column: denied.
------------------------------------------------------------------
SET ROLE bm25_wrongcol;
SELECT 'alpha'::text <@>
       (SELECT to_bm25query('alpha', 'victim.docs_bm25')) AS wrongcol_present;
RESET ROLE;

------------------------------------------------------------------
-- Reader with table SELECT but no schema USAGE: denied at the schema.
------------------------------------------------------------------
SET ROLE bm25_nousage;
SELECT 'alpha'::text <@>
       (SELECT to_bm25query('alpha', 'victim.docs_bm25')) AS nousage_present;
RESET ROLE;

------------------------------------------------------------------
-- Expression index: a role missing SELECT on any referenced column is
-- denied; granting the remaining column allows scoring.
------------------------------------------------------------------
SET ROLE bm25_exprreader;
SELECT 'alpha'::text <@>
       (SELECT to_bm25query('alpha', 'victim.expr_bm25')) AS expr_denied;
RESET ROLE;
GRANT SELECT (b) ON victim.expr_docs TO bm25_exprreader;
SET ROLE bm25_exprreader;
SELECT ('alpha'::text <@>
        (SELECT to_bm25query('alpha', 'victim.expr_bm25'))) < 0
    AS expr_allowed;
RESET ROLE;

------------------------------------------------------------------
-- Partial index: a role missing SELECT on the predicate column is
-- denied; granting it allows scoring.
------------------------------------------------------------------
SET ROLE bm25_predreader;
SELECT 'alpha'::text <@>
       (SELECT to_bm25query('alpha', 'victim.part_bm25')) AS pred_denied;
RESET ROLE;
GRANT SELECT (flag) ON victim.part_docs TO bm25_predreader;
SET ROLE bm25_predreader;
SELECT ('alpha'::text <@>
        (SELECT to_bm25query('alpha', 'victim.part_bm25'))) < 0
    AS pred_allowed;
RESET ROLE;

------------------------------------------------------------------
-- Reader with full table SELECT: scoring works.
------------------------------------------------------------------
SET ROLE bm25_reader;
SELECT ('alpha'::text <@>
        (SELECT to_bm25query('alpha', 'victim.docs_bm25'))) < 0
    AS reader_present;
SELECT 'zzzabsent'::text <@>
       (SELECT to_bm25query('zzzabsent', 'victim.docs_bm25')) = 0
    AS reader_absent;
RESET ROLE;

------------------------------------------------------------------
-- Reader with column-level SELECT on the indexed column: scoring works.
------------------------------------------------------------------
SET ROLE bm25_colreader;
SELECT ('alpha'::text <@>
        (SELECT to_bm25query('alpha', 'victim.docs_bm25'))) < 0
    AS colreader_present;
RESET ROLE;

-- Cleanup
DROP SCHEMA victim CASCADE;
DROP OWNED BY bm25_attacker, bm25_reader, bm25_colreader,
              bm25_wrongcol, bm25_nousage, bm25_exprreader,
              bm25_predreader CASCADE;
DROP ROLE bm25_attacker;
DROP ROLE bm25_reader;
DROP ROLE bm25_colreader;
DROP ROLE bm25_wrongcol;
DROP ROLE bm25_nousage;
DROP ROLE bm25_exprreader;
DROP ROLE bm25_predreader;
DROP EXTENSION pg_textsearch;
