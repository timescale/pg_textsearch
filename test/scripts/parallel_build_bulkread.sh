#!/bin/bash
# Parallel builds should reuse a bulk-read ring, not fill shared_buffers.
set -euo pipefail

PG_CONFIG="${PG_CONFIG:-pg_config}"
export PATH="$("${PG_CONFIG}" --bindir):${PATH}"
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pgts_bulkread.XXXXXX")
DATA_DIR="${TEST_DIR}/data"
export PGHOST="${TEST_DIR}"
export PGPORT="${TEST_PORT:-55479}"
export PGDATABASE=postgres

cleanup() {
    local status=$?
    trap - EXIT
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        if ! pg_ctl stop -D "${DATA_DIR}" -m fast -w >/dev/null; then
            status=1
        fi
    fi
    if [ "${status}" -eq 0 ]; then
        rm -rf "${TEST_DIR}"
    else
        echo "FAIL: evidence preserved in ${TEST_DIR}" >&2
    fi
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

sql() {
    psql -XqAt -v ON_ERROR_STOP=1 "$@"
}

initdb -D "${DATA_DIR}" -A trust --no-locale >/dev/null
pg_ctl start -D "${DATA_DIR}" -l "${TEST_DIR}/server.log" -w \
    -o "-k ${PGHOST} -p ${PGPORT} -c listen_addresses='' \
        -c shared_preload_libraries=pg_textsearch \
        -c shared_buffers=16MB -c max_worker_processes=4 \
        -c max_parallel_workers=2 \
        -c max_parallel_maintenance_workers=2" >/dev/null

sql >"${TEST_DIR}/fixture.log" 2>&1 <<'SQL'
CREATE EXTENSION pg_textsearch;
CREATE TABLE docs (id integer, body text) WITH (parallel_workers=2);
INSERT INTO docs
SELECT i, repeat('database search benchmark ', 12)
FROM generate_series(1, 150000) i;
ANALYZE docs;
DO $$
BEGIN
    IF pg_relation_size('docs') <=
       pg_size_bytes(current_setting('shared_buffers')) / 4 THEN
        RAISE EXCEPTION 'fixture too small for bulk-read strategy';
    END IF;
END
$$;
SELECT pg_stat_reset_shared('io');
SQL

sql >"${TEST_DIR}/build.log" 2>&1 <<'SQL'
SET maintenance_work_mem = '64MB';
CREATE INDEX docs_bm25 ON docs USING bm25(body)
    WITH (text_config='english');
SET enable_seqscan = off;
DO $$
DECLARE
    hits bigint;
    distinct_ids bigint;
BEGIN
    SELECT count(*), count(DISTINCT id) INTO hits, distinct_ids
    FROM (
        SELECT id FROM docs
        ORDER BY body <@> to_bm25query('database', 'docs_bm25')
        LIMIT 150000
    ) ranked;
    IF hits <> 150000 OR distinct_ids <> 150000 THEN
        RAISE EXCEPTION 'incorrect build: % hits, % distinct IDs',
            hits, distinct_ids;
    END IF;
END
$$;
SQL

grep -Fq 'launched 2 of 2 requested workers' "${TEST_DIR}/build.log" || {
    echo "ERROR: expected two parallel build workers" >&2
    exit 1
}

# pg_stat_io groups parallel workers under "background worker".
# Their statistics can arrive just after CREATE INDEX returns.
for attempt in $(seq 1 100); do
    reuses=$(sql -c "SELECT coalesce(sum(reuses), 0)
        FROM pg_stat_io
        WHERE backend_type = 'background worker'
          AND object = 'relation' AND context = 'bulkread'")
    if [ "${reuses}" -gt 0 ]; then
        echo "PASS: parallel workers reused bulk-read buffers (${reuses})"
        exit 0
    fi
    sleep 0.1
done
sql -c "SELECT * FROM pg_stat_io WHERE backend_type='background worker'" \
    >"${TEST_DIR}/io.log"
echo "ERROR: parallel build did not reuse bulk-read buffers" >&2
exit 1
