#!/bin/bash
# A rolled-back DROP must not invalidate another backend's cached state (#506).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TEST_DIR="$(mktemp -d "${ROOT}/test/tmp_drop_rollback.XXXXXX")"
SOCKET_DIR="$(mktemp -d /tmp/pgts-drop.XXXXXX)"
export PGHOST="${SOCKET_DIR}"
export PGPORT="${TEST_PORT:-55463}"
export PGDATABASE=postgres

cleanup() {
    local status=$?
    trap - EXIT
    if [ -f "${TEST_DIR}/data/postmaster.pid" ]; then
        if ! pg_ctl -D "${TEST_DIR}/data" -m immediate -w stop >/dev/null; then
            echo "Could not stop test server; preserving ${TEST_DIR}" >&2
            exit 1
        fi
    fi
    if [ "${status}" -ne 0 ]; then
        cat "${TEST_DIR}/postgres.log" >&2
    fi
    rm -rf "${TEST_DIR}" "${SOCKET_DIR}"
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

initdb -D "${TEST_DIR}/data" --auth=trust >/dev/null
cat >>"${TEST_DIR}/data/postgresql.conf" <<EOF
listen_addresses = ''
unix_socket_directories = '${SOCKET_DIR}'
port = ${PGPORT}
shared_preload_libraries = 'pg_textsearch'
restart_after_crash = off
max_prepared_transactions = 10
pg_textsearch.bulk_load_threshold = 0
pg_textsearch.memtable_pages_threshold = 0
statement_timeout = '30s'
EOF
pg_ctl -D "${TEST_DIR}/data" -l "${TEST_DIR}/postgres.log" -w start

sql() {
    psql -X -v ON_ERROR_STOP=1 "$@"
}

sql <<'SQL'
CREATE EXTENSION pg_textsearch;
CREATE FUNCTION assert_hits(expected bigint) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
    actual bigint;
BEGIN
    SELECT count(*) INTO actual FROM (
        SELECT id FROM docs
        ORDER BY txt <@> to_bm25query('drug trial', 'docs_idx')
        LIMIT 30000
    ) hits;
    IF actual <> expected THEN
        RAISE EXCEPTION 'expected % hits, got %', expected, actual;
    END IF;
END;
$$;
SQL

cat >"${TEST_DIR}/writer.sql" <<'SQL'
INSERT INTO docs
SELECT i, 'drug trial late ' || i FROM generate_series(20002, 20101) i;
SQL

fixture() {
    sql <<'SQL'
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id int, txt text);
INSERT INTO docs
SELECT i, 'drug trial phase ' || i || ' result ' || md5(i::text)
FROM generate_series(1, 20000) i;
CREATE INDEX docs_idx ON docs USING bm25 (txt)
WITH (text_config = 'english');
INSERT INTO docs VALUES (20001, 'drug trial cached');
SQL
}

rollback_case() {
    local name="$1" ddl="$2"
    echo "Testing ${name}"
    fixture
    printf '%s\n' "${ddl}" >"${TEST_DIR}/drop.sql"
    # \! runs the dropper and writer while the original reader stays connected.
    sql <<SQL
SELECT assert_hits(20001);
SELECT bm25_cache_global_estimated_bytes() AS cache_bytes \gset
\\! psql -X -v ON_ERROR_STOP=1 -f "${TEST_DIR}/drop.sql"
\\if :SHELL_ERROR
    DO \$\$ BEGIN RAISE EXCEPTION 'dropper failed'; END \$\$;
\\endif
-- Catch the premature free even when DSA happens to reuse the same address.
SELECT :cache_bytes > 0 AND
       bm25_cache_global_estimated_bytes() = :cache_bytes AS retained \gset
\\if :retained
\\else
    DO \$\$ BEGIN RAISE EXCEPTION 'rollback freed shared cache'; END \$\$;
\\endif
\\! psql -X -v ON_ERROR_STOP=1 -f "${TEST_DIR}/writer.sql"
\\if :SHELL_ERROR
    DO \$\$ BEGIN RAISE EXCEPTION 'writer failed'; END \$\$;
\\endif
SELECT assert_hits(20101);
-- The dropping backend must also retain a usable wrapper after rollback.
BEGIN;
DROP INDEX docs_idx;
ROLLBACK;
SELECT assert_hits(20101);
SQL
}

rollback_case "transaction rollback" \
    "BEGIN; DROP INDEX docs_idx; ROLLBACK;"
rollback_case "savepoint rollback followed by commit" \
    "BEGIN; SAVEPOINT s; DROP INDEX docs_idx; ROLLBACK TO s; COMMIT;"
rollback_case "released savepoint followed by rollback" \
    "BEGIN; SAVEPOINT s; DROP INDEX docs_idx; RELEASE s; ROLLBACK;"
rollback_case "nested release followed by parent rollback" \
    "BEGIN; SAVEPOINT a; SAVEPOINT b; DROP INDEX docs_idx;\
     RELEASE b; ROLLBACK TO a; COMMIT;"
rollback_case "cascading table drop rollback" \
    "BEGIN; DROP TABLE docs; ROLLBACK;"
rollback_case "extension drop rollback" \
    "BEGIN; DROP EXTENSION pg_textsearch CASCADE; ROLLBACK;"
rollback_case "caught statement error" \
    'DO $$ BEGIN
         DROP INDEX docs_idx;
         PERFORM 1 / 0;
     EXCEPTION WHEN division_by_zero THEN NULL;
     END $$;'

echo "Testing PREPARE rejection and savepoint queue cleanup"
fixture
sql <<'SQL'
SELECT assert_hits(20001);
BEGIN;
DROP INDEX docs_idx;
\set ON_ERROR_STOP off
PREPARE TRANSACTION 'drop_rejected';
\set prepare_state :SQLSTATE
\set ON_ERROR_STOP on
ROLLBACK;
SELECT :'prepare_state' = '0A000' AS rejected \gset
\if :rejected
\else
    DO $$ BEGIN RAISE EXCEPTION 'PREPARE accepted pending drop'; END $$;
\endif
SELECT assert_hits(20001);
BEGIN;
SAVEPOINT s;
DROP INDEX docs_idx;
ROLLBACK TO s;
PREPARE TRANSACTION 'drop_rolled_back';
COMMIT PREPARED 'drop_rolled_back';
SELECT assert_hits(20001);
SQL

echo "Testing committed drop cleanup and create/drop ownership"
for ddl in \
    "DROP INDEX docs_idx" \
    "DROP INDEX CONCURRENTLY docs_idx" \
    "DROP TABLE docs" \
    "BEGIN; SAVEPOINT a; SAVEPOINT b; DROP INDEX docs_idx;\
     RELEASE b; RELEASE a; COMMIT"
do
    fixture
    sql <<SQL
SELECT assert_hits(20001);
SELECT bm25_cache_global_estimated_bytes() > 0 AS populated \\gset
\\if :populated
\\else
    DO \$\$ BEGIN RAISE EXCEPTION 'cache was not populated'; END \$\$;
\\endif
${ddl};
DO \$\$
BEGIN
    IF bm25_cache_global_estimated_bytes() <> 0 THEN
        RAISE EXCEPTION 'committed drop leaked cache memory';
    END IF;
END;
\$\$;
SQL
done

echo "Testing mixed parent and savepoint drops"
fixture
sql <<'SQL'
CREATE TABLE survivor (txt text);
CREATE INDEX survivor_idx ON survivor USING bm25 (txt)
WITH (text_config = 'english');
INSERT INTO survivor VALUES ('drug trial');
SELECT txt FROM survivor
ORDER BY txt <@> to_bm25query('drug trial', 'survivor_idx') LIMIT 1;
SELECT bm25_cache_global_estimated_bytes() AS survivor_bytes \gset
SELECT assert_hits(20001);
BEGIN;
DROP INDEX docs_idx;
SAVEPOINT s;
DROP INDEX survivor_idx;
ROLLBACK TO s;
COMMIT;
SELECT bm25_cache_global_estimated_bytes() = :survivor_bytes AS retained \gset
\if :retained
\else
    DO $$ BEGIN RAISE EXCEPTION 'wrong drop survived savepoint'; END $$;
\endif
SELECT txt FROM survivor
ORDER BY txt <@> to_bm25query('drug trial', 'survivor_idx') LIMIT 1;
CREATE INDEX docs_idx ON docs USING bm25 (txt)
WITH (text_config = 'english');
INSERT INTO docs VALUES (20002, 'drug trial');
SELECT assert_hits(20002);
DROP TABLE docs, survivor;
DO $$
BEGIN
    IF bm25_cache_global_estimated_bytes() <> 0 THEN
        RAISE EXCEPTION 'multiple drops leaked cache memory';
    END IF;
END;
$$;
SQL

sql <<'SQL'
DROP TABLE IF EXISTS docs;
CREATE TABLE docs (id int, txt text);
INSERT INTO docs VALUES (1, 'drug trial');
BEGIN;
CREATE INDEX docs_idx ON docs USING bm25 (txt)
WITH (text_config = 'english');
INSERT INTO docs VALUES (2, 'drug trial');
SELECT assert_hits(2);
DROP INDEX docs_idx;
ROLLBACK;
BEGIN;
CREATE INDEX docs_idx ON docs USING bm25 (txt)
WITH (text_config = 'english');
INSERT INTO docs VALUES (2, 'drug trial');
SELECT assert_hits(2);
DROP INDEX docs_idx;
COMMIT;
BEGIN;
PREPARE TRANSACTION 'after_drop';
COMMIT PREPARED 'after_drop';
DO $$
BEGIN
    IF bm25_cache_global_estimated_bytes() <> 0 THEN
        RAISE EXCEPTION 'create/drop leaked cache memory';
    END IF;
END;
$$;
SQL
echo "DROP rollback tests passed"
