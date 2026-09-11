#!/bin/bash
#
# Exercise Boolean segment snapshots while spill, compaction, and VACUUM
# replace and reclaim the captured segment roots.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55460
TEST_DB=boolean_concurrent_merge_test
DATA_DIR="${SCRIPT_DIR}/../tmp_boolean_concurrent_merge"
LOGFILE="${DATA_DIR}/postgres.log"
ERR_DIR="${DATA_DIR}/client_logs"

cleanup() {
    local exit_code=$?

    jobs -p | xargs -r kill 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    if [ "$exit_code" -ne 0 ]; then
        tail -n 40 "${ERR_DIR}"/*.log "${LOGFILE}" 2>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit "$exit_code"
}

trap cleanup EXIT INT TERM

rm -rf "${DATA_DIR}"
mkdir -p "${DATA_DIR}"
initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null
mkdir -p "${ERR_DIR}"
cat >>"${DATA_DIR}/postgresql.conf" <<EOF
port = ${TEST_PORT}
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
shared_preload_libraries = 'pg_textsearch'
default_text_search_config = 'pg_catalog.simple'
EOF
pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w -o "-p ${TEST_PORT}" \
    >/dev/null
createdb -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"

PSQL=(
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}"
    -qAt -v ON_ERROR_STOP=1
)

"${PSQL[@]}" <<'SQL' >/dev/null
CREATE EXTENSION pg_textsearch;
CREATE TABLE docs (id bigserial PRIMARY KEY, body text NOT NULL);
CREATE INDEX docs_bm25 ON docs USING bm25(body)
    WITH (text_config='simple');
SET pg_textsearch.segments_per_level = 64;
DO $$
BEGIN
    FOR batch IN 1..16 LOOP
        INSERT INTO docs (body)
        SELECT 'common token ' || batch || ' ' || gs
        FROM generate_series(1, 500) gs;
        PERFORM bm25_spill_index('docs_bm25');
    END LOOP;
END
$$;
SQL

plan="$("${PSQL[@]}" -c "
    SET enable_seqscan=off;
    EXPLAIN (COSTS off)
    SELECT count(*) FROM docs
    WHERE body @@ to_tsquery('simple', '!missing')")"
if ! grep -q "Index Scan using docs_bm25" <<<"${plan}"; then
    echo "Boolean concurrency test did not select docs_bm25" >&2
    echo "${plan}" >&2
    exit 1
fi

reader() {
    local tag=$1
    local result

    for _ in $(seq 1 40); do
        result="$("${PSQL[@]}" -c "
            SET enable_seqscan=off;
            SET statement_timeout='60s';
            SELECT (SELECT count(*) FROM docs) =
                   (SELECT count(*) FROM docs
                    WHERE body @@ to_tsquery('simple', '!missing'))" \
            2>>"${ERR_DIR}/reader_${tag}.log")"
        if [ "${result}" != "t" ]; then
            echo "Boolean result mismatch: ${result}" \
                >>"${ERR_DIR}/reader_${tag}.log"
            return 40
        fi
    done
}

writer() {
    for batch in $(seq 1 40); do
        "${PSQL[@]}" -c "
            SET pg_textsearch.segments_per_level=64;
            INSERT INTO docs (body)
            SELECT 'common writer ${batch} ' || gs
            FROM generate_series(1, 100) gs;
            SELECT bm25_spill_index('docs_bm25')" \
            >>"${ERR_DIR}/writer.log" 2>&1
    done
}

merger() {
    for _ in $(seq 1 50); do
        "${PSQL[@]}" -c "
            SET pg_textsearch.segments_per_level=2;
            SET statement_timeout='60s';
            SELECT bm25_force_merge('docs_bm25');
            SELECT bm25_pending_free_pages('docs_bm25')" \
            >>"${ERR_DIR}/merger.log" 2>&1
        "${PSQL[@]}" -c "VACUUM docs" >>"${ERR_DIR}/merger.log" 2>&1
    done
}

reader a & reader_a=$!
reader b & reader_b=$!
writer & writer_pid=$!
merger & merger_pid=$!

failed=0
wait "${reader_a}" || failed=1
wait "${reader_b}" || failed=1
wait "${writer_pid}" || failed=1
wait "${merger_pid}" || failed=1

if grep -REq \
    "invalid segment header|could not open BM25 segment|terminated by signal" \
    "${ERR_DIR}" "${LOGFILE}"; then
    echo "Boolean scan observed a reclaimed or invalid segment" >&2
    exit 1
fi
if [ "${failed}" -ne 0 ]; then
    echo "A Boolean concurrency worker failed" >&2
    exit 1
fi
if ! grep -Eq '^[1-9][0-9]*$' "${ERR_DIR}/merger.log"; then
    echo "Concurrency test did not observe displaced segment pages" >&2
    exit 1
fi

expected="$("${PSQL[@]}" -c "SELECT count(*) FROM docs")"
actual="$("${PSQL[@]}" -c "
    SET enable_seqscan=off;
    SELECT count(*) FROM docs
    WHERE body @@ to_tsquery('simple', '!missing')")"
if [ "${actual}" != "${expected}" ]; then
    echo "Boolean result mismatch: expected ${expected}, got ${actual}" >&2
    exit 1
fi

echo "Boolean scans survived concurrent spill, merge, and VACUUM"
