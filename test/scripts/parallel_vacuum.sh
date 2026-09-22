#!/bin/bash
#
# Parallel VACUUM enters parallel mode before calling index bulk-delete,
# including for indexes processed by the leader.  pg_textsearch maintenance
# must therefore avoid assigning a transaction ID while removing dead docs.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"
export PATH="${PGBINDIR}:${PATH}"
TEST_PORT=55461
TEST_DB=parallel_vacuum_test
DATA_DIR="${SCRIPT_DIR}/../tmp_parallel_vacuum"
SOCKET_DIR="${REPO_ROOT}/.parallel_vacuum_sock"
LOGFILE="${DATA_DIR}/postgres.log"
MODE="${1:-normal}"
PRELOAD_LIBRARIES=pg_textsearch

if [ "${MODE}" = "injection" ]; then
    [ -f "$("${PG_CONFIG}" --pkglibdir)/injection_points.so" ] ||
        {
            echo "injection_points.so is required for injection mode" >&2
            exit 1
        }
    PRELOAD_LIBRARIES='pg_textsearch,injection_points'
fi

cleanup() {
    local exit_code=$?

    trap - EXIT INT TERM
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate >/dev/null 2>&1 || true
    fi
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    exit "${exit_code}"
}

trap cleanup EXIT INT TERM

rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
mkdir -p "${DATA_DIR}" "${SOCKET_DIR}"

initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
    >/dev/null 2>&1
cat >>"${DATA_DIR}/postgresql.conf" <<EOF
port = ${TEST_PORT}
unix_socket_directories = '${SOCKET_DIR}'
listen_addresses = 'localhost'
shared_preload_libraries = '${PRELOAD_LIBRARIES}'
logging_collector = on
log_directory = '.'
log_filename = 'postgres.log'
autovacuum = off
EOF

if ! pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null; then
    cat "${LOGFILE}" >&2
    exit 1
fi
createdb -h "${SOCKET_DIR}" -p "${TEST_PORT}" "${TEST_DB}"

PSQL=(
    psql
    -h "${SOCKET_DIR}"
    -p "${TEST_PORT}"
    -d "${TEST_DB}"
    -qAt
    -v ON_ERROR_STOP=1
)

"${PSQL[@]}" <<'SQL' >/dev/null
CREATE EXTENSION pg_textsearch;
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 0;

CREATE TABLE spill_docs (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    body text NOT NULL
);
CREATE INDEX spill_docs_bm25 ON spill_docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'off');
DO $$
BEGIN
    FOR batch IN 1..8 LOOP
        INSERT INTO spill_docs (body)
        SELECT 'parallel spill batch ' || batch || ' document ' || gs
        FROM generate_series(1, 500) gs;
        PERFORM bm25_spill_index('spill_docs_bm25');
    END LOOP;
END
$$;
ALTER INDEX spill_docs_bm25 SET (compaction = 'inline');
INSERT INTO spill_docs (body)
SELECT 'parallel spill pending document ' || gs || ' ' ||
       repeat(md5(gs::text), 4)
FROM generate_series(1, 20000) gs;
DELETE FROM spill_docs WHERE id <= 5000;

CREATE TABLE docs (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    body text NOT NULL
);
CREATE INDEX docs_bm25 ON docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'off');
INSERT INTO docs (body)
SELECT 'parallel vacuum document ' || gs || ' ' || repeat(md5(gs::text), 4)
FROM generate_series(1, 20000) gs;
SELECT bm25_spill_index('docs_bm25');
DELETE FROM docs;
SQL

if [ "${MODE}" = "injection" ]; then
    "${PSQL[@]}" <<'SQL' >/dev/null
CREATE EXTENSION injection_points;
CREATE EXTENSION pg_textsearch_test;
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 0;
CREATE TABLE legacy_docs (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    body text NOT NULL
);
CREATE INDEX legacy_docs_bm25 ON legacy_docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'off');
INSERT INTO legacy_docs (body)
SELECT 'parallel legacy document ' || gs || ' ' ||
       repeat(md5(gs::text), 4)
FROM generate_series(1, 20000) gs;
SELECT pg_textsearch_test_attach_legacy_segment(1000000);
SELECT bm25_spill_index('legacy_docs_bm25');
SELECT injection_points_detach('pg-textsearch-legacy-segment');
SQL

    legacy_root=$(
        "${PSQL[@]}" -c \
            "SELECT (regexp_match(
                bm25_summarize_index('legacy_docs_bm25'),
                'L0 Segment 1: block=([0-9]+)'))[1];"
    )
    "${PSQL[@]}" -c \
        "DELETE FROM legacy_docs WHERE id <= 5000;" >/dev/null
    legacy_pending_before=$(
        "${PSQL[@]}" -c \
            "SELECT bm25_pending_free_pages('legacy_docs_bm25');"
    )

    if ! legacy_vacuum_output=$(
        "${PSQL[@]}" <<'SQL' 2>&1
SET min_parallel_index_scan_size = 0;
VACUUM (PARALLEL 1, VERBOSE) legacy_docs;
SQL
    ); then
        echo "${legacy_vacuum_output}" >&2
        exit 1
    fi

    if ! grep -q "launched 1 parallel vacuum worker" \
        <<<"${legacy_vacuum_output}"; then
        echo "parallel legacy VACUUM did not launch a worker" >&2
        echo "${legacy_vacuum_output}" >&2
        exit 1
    fi

    legacy_remaining=$(
        "${PSQL[@]}" -c "
            SELECT count(*)
              FROM (
                    SELECT id
                      FROM legacy_docs
                     ORDER BY body <@> to_bm25query(
                             'parallel legacy', 'legacy_docs_bm25')
                   ) ranked;"
    )
    if [ "${legacy_remaining}" != "15000" ]; then
        echo "parallel legacy VACUUM retained ${legacy_remaining}/15000 documents" \
            >&2
        exit 1
    fi

    legacy_summary=$(
        "${PSQL[@]}" -c \
            "SELECT bm25_summarize_index('legacy_docs_bm25');"
    )
    legacy_new_root=$(
        sed -n 's/.*L0 Segment 1: block=\([0-9][0-9]*\).*/\1/p' \
            <<<"${legacy_summary}"
    )
    if [ -z "${legacy_new_root}" ]; then
        echo "parallel legacy VACUUM summary has no L0 segment root" >&2
        echo "${legacy_summary}" >&2
        exit 1
    fi
    if [ "${legacy_new_root}" = "${legacy_root}" ]; then
        echo "parallel legacy VACUUM did not replace root ${legacy_root}" >&2
        exit 1
    fi

    legacy_pending_after=$(
        "${PSQL[@]}" -c \
            "SELECT bm25_pending_free_pages('legacy_docs_bm25');"
    )
    if [ "${legacy_pending_after}" -le "${legacy_pending_before}" ]; then
        echo "parallel legacy VACUUM pending-free pages did not increase: " \
            "${legacy_pending_before} -> ${legacy_pending_after}" >&2
        exit 1
    fi
fi

spill_memtable_before=$(
    "${PSQL[@]}" -c \
        "SELECT count(*) FROM bm25_memtable_chain('spill_docs_bm25');"
)
if [ "${spill_memtable_before}" -le 0 ]; then
    echo "parallel spill VACUUM setup has no pending memtable pages" >&2
    exit 1
fi

if ! spill_vacuum_output=$(
    "${PSQL[@]}" <<'SQL' 2>&1
SET min_parallel_index_scan_size = 0;
VACUUM (PARALLEL 1, VERBOSE) spill_docs;
SQL
); then
    echo "${spill_vacuum_output}" >&2
    exit 1
fi

if ! grep -q "launched 1 parallel vacuum worker" \
    <<<"${spill_vacuum_output}"; then
    echo "parallel spill VACUUM did not launch a worker" >&2
    echo "${spill_vacuum_output}" >&2
    exit 1
fi

spill_memtable_after=$(
    "${PSQL[@]}" -c \
        "SELECT count(*) FROM bm25_memtable_chain('spill_docs_bm25');"
)
if [ "${spill_memtable_after}" -ne 0 ]; then
    echo "parallel VACUUM did not spill ${spill_memtable_after} memtable pages" \
        >&2
    exit 1
fi

spill_remaining=$(
    "${PSQL[@]}" -c "
        SELECT count(*)
          FROM (
                SELECT id
                  FROM spill_docs
                 ORDER BY body <@> to_bm25query(
                         'parallel spill', 'spill_docs_bm25')
               ) ranked;"
)

if [ "${spill_remaining}" != "19000" ]; then
    echo "parallel VACUUM spill retained ${spill_remaining}/19000 documents" \
        >&2
    exit 1
fi

if ! vacuum_output=$(
    "${PSQL[@]}" <<'SQL' 2>&1
SET min_parallel_index_scan_size = 0;
VACUUM (PARALLEL 1, VERBOSE) docs;
SQL
); then
    echo "${vacuum_output}" >&2
    exit 1
fi

if ! grep -q "launched 1 parallel vacuum worker" <<<"${vacuum_output}"; then
    echo "parallel VACUUM did not launch a worker" >&2
    echo "${vacuum_output}" >&2
    exit 1
fi

remaining=$(
    "${PSQL[@]}" -c "
        SELECT count(*)
          FROM (
                SELECT id
                  FROM docs
                 ORDER BY body <@> to_bm25query(
                         'parallel vacuum', 'docs_bm25')
               ) ranked;"
)

if [ "${remaining}" != "0" ]; then
    echo "parallel VACUUM left ${remaining} deleted documents searchable" >&2
    exit 1
fi

parallel_levels=$(
    "${PSQL[@]}" -c "SELECT bm25_level_counts('docs_bm25');"
)
if [ "${parallel_levels}" != "{1,0,0,0,0,0,0,0}" ]; then
    echo "parallel VACUUM left unexpected levels ${parallel_levels}" >&2
    exit 1
fi

parallel_summary=$(
    "${PSQL[@]}" -c "SELECT bm25_summarize_index('docs_bm25');"
)
if ! grep -q "alive=0, dead=20000" <<<"${parallel_summary}"; then
    echo "parallel VACUUM did not persist the all-dead alive bitmap" >&2
    echo "${parallel_summary}" >&2
    exit 1
fi

"${PSQL[@]}" -c "VACUUM (PARALLEL 0) docs;" >/dev/null

serial_levels=$(
    "${PSQL[@]}" -c "SELECT bm25_level_counts('docs_bm25');"
)
if [ "${serial_levels}" != "{0,0,0,0,0,0,0,0}" ]; then
    echo "serial VACUUM left empty segment levels ${serial_levels}" >&2
    exit 1
fi

"${PSQL[@]}" <<'SQL' >/dev/null
ALTER TABLE docs ADD COLUMN label text;
CREATE TABLE control_docs (
    label text NOT NULL,
    body text NOT NULL
);
CREATE INDEX control_docs_bm25 ON control_docs USING bm25(body)
    WITH (text_config = 'english', compaction = 'off');

INSERT INTO docs (label, body) VALUES
    ('short', 'needle alpha beta gamma delta'),
    ('long', 'needle needle needle needle needle' ||
             repeat(' filler', 45));
INSERT INTO control_docs SELECT label, body FROM docs;
SELECT bm25_spill_index('docs_bm25');
SELECT bm25_spill_index('control_docs_bm25');
SQL

cleanup_order=$(
    "${PSQL[@]}" -c "
        SELECT string_agg(label, ',' ORDER BY score, label)
          FROM (
                SELECT label,
                       body <@> to_bm25query(
                           'needle', 'docs_bm25') AS score
                  FROM docs
                 ORDER BY body <@> to_bm25query(
                              'needle', 'docs_bm25')
               ) ranked;"
)
control_order=$(
    "${PSQL[@]}" -c "
        SELECT string_agg(label, ',' ORDER BY score, label)
          FROM (
                SELECT label,
                       body <@> to_bm25query(
                           'needle', 'control_docs_bm25') AS score
                  FROM control_docs
                 ORDER BY body <@> to_bm25query(
                              'needle', 'control_docs_bm25')
               ) ranked;"
)

if [ "${control_order}" != "long,short" ]; then
    echo "control corpus did not produce sensitive order: ${control_order}" >&2
    exit 1
fi
if [ "${cleanup_order}" != "${control_order}" ]; then
    echo "post-cleanup order ${cleanup_order} differs from control ${control_order}" \
        >&2
    exit 1
fi

echo "Parallel VACUUM test passed"
