#!/bin/bash
#
# Verify that runtime spill construction blocks memtable writers, not readers.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"
export PATH="${PGBINDIR}:${PATH}"
TEST_PORT=55468
TEST_DB=nonblocking_spill_test
DATA_DIR="${SCRIPT_DIR}/../tmp_nonblocking_spill"
SOCKET_DIR="${SCRIPT_DIR}/.nbs_sock"
LOGFILE="${DATA_DIR}/postgres.log"
CLIENT_DIR="${DATA_DIR}/clients"
POINT_SPILL_BEFORE_FINALIZE='pg-textsearch-spill-before-finalize'
POINT_TOMBSTONE_AFTER_UNLINK='pg-textsearch-tombstone-after-unlink'

fail() {
    echo "ERROR: $*" >&2
    tail -n 80 "${LOGFILE}" 2>/dev/null || true
    for file in "${CLIENT_DIR}"/*.log; do
        [ -e "${file}" ] || continue
        echo "==> ${file} <==" >&2
        tail -n 30 "${file}" >&2 || true
    done
    exit 1
}

cleanup() {
    local status=$?
    local pid

    trap - EXIT INT TERM
    for pid in $(jobs -p); do
        kill "${pid}" 2>/dev/null || true
    done
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate >/dev/null 2>&1 || true
    fi
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    exit "${status}"
}

trap cleanup EXIT INT TERM

sql() {
    psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -qAt -v ON_ERROR_STOP=1 "$@"
}

backend_pid() {
    local app_name=$1
    local deadline=$((SECONDS + 10))
    local pid

    while ((SECONDS < deadline)); do
        pid=$(sql -c "
            SELECT pid
            FROM pg_stat_activity
            WHERE application_name = '${app_name}'
            ORDER BY backend_start DESC
            LIMIT 1;" 2>/dev/null || true)
        if [[ "${pid}" =~ ^[0-9]+$ ]]; then
            echo "${pid}"
            return
        fi
        sleep 0.05
    done
    fail "backend ${app_name} did not appear"
}

wait_for_injection() {
    local backend=$1
    local deadline=$((SECONDS + 10))

    while ((SECONDS < deadline)); do
        if [ "$(sql -c "
            SELECT EXISTS (
                SELECT 1
                FROM pg_stat_activity
                WHERE pid = ${backend}
                  AND wait_event_type = 'InjectionPoint'
                  AND wait_event = '${POINT_SPILL_BEFORE_FINALIZE}'
            );")" = "t" ]; then
            return
        fi
        sleep 0.05
    done
    fail "spill did not reach ${POINT_SPILL_BEFORE_FINALIZE}"
}

wait_for_wait_event() {
    local backend=$1
    local wait_event=$2
    local deadline=$((SECONDS + 10))

    while ((SECONDS < deadline)); do
        if [ "$(sql -c "
            SELECT wait_event
            FROM pg_stat_activity
            WHERE pid = ${backend};")" = "${wait_event}" ]; then
            return
        fi
        sleep 0.05
    done
    fail "backend ${backend} did not wait on ${wait_event}"
}

wait_for_exit() {
    local pid=$1
    local seconds=$2
    local label=$3
    local deadline=$((SECONDS + seconds))

    while kill -0 "${pid}" 2>/dev/null; do
        ((SECONDS < deadline)) || fail "${label} did not finish"
        sleep 0.05
    done
    wait "${pid}" || fail "${label} failed"
}

rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
mkdir -p "${SOCKET_DIR}"
initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null
mkdir -p "${CLIENT_DIR}"
cat >>"${DATA_DIR}/postgresql.conf" <<EOF
port = ${TEST_PORT}
unix_socket_directories = '${SOCKET_DIR}'
listen_addresses = ''
shared_preload_libraries = 'pg_textsearch'
autovacuum = off
pg_textsearch.memtable_pages_threshold = 0
pg_textsearch.bulk_load_threshold = 0
pg_textsearch.segments_per_level = 2
EOF
pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null ||
    fail "PostgreSQL startup failed"
createdb -h "${SOCKET_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
sql -c "
    CREATE EXTENSION pg_textsearch;
    CREATE EXTENSION injection_points;
    CREATE TABLE spill_docs (id integer PRIMARY KEY, body text NOT NULL);
    CREATE INDEX spill_idx ON spill_docs USING bm25(body)
        WITH (text_config = 'english', compaction = 'manual');
    INSERT INTO spill_docs
    SELECT gs, 'alpha document ' || gs
    FROM generate_series(1, 500) gs;" >/dev/null

PGAPPNAME=spill-builder sql -c "
    SET statement_timeout = '60s';
    SELECT injection_points_set_local();
    SELECT injection_points_attach(
        '${POINT_SPILL_BEFORE_FINALIZE}', 'wait');
    SELECT bm25_spill_index('spill_idx');" \
    >"${CLIENT_DIR}/spill.log" 2>&1 &
spill_client=$!
spill_backend=$(backend_pid spill-builder)
wait_for_injection "${spill_backend}"

PGAPPNAME=spill-reader sql -c "
    SELECT count(*)
    FROM (
        SELECT 1
        FROM spill_docs
        ORDER BY body <@> to_bm25query('alpha', 'spill_idx')
        LIMIT 500
    ) ranked;" >"${CLIENT_DIR}/reader.log" 2>&1 &
reader_client=$!
wait_for_exit "${reader_client}" 3 "ranked reader"
grep -qx '500' "${CLIENT_DIR}/reader.log" ||
    fail "ranked reader did not see the stable chain"

PGAPPNAME=spill-writer sql -c "
    INSERT INTO spill_docs VALUES (501, 'alpha document 501');" \
    >"${CLIENT_DIR}/writer.log" 2>&1 &
writer_client=$!
writer_backend=$(backend_pid spill-writer)
wait_for_wait_event "${writer_backend}" tapir_memtable_write_lock

sql -c "SELECT injection_points_wakeup(
    '${POINT_SPILL_BEFORE_FINALIZE}');" >/dev/null
wait_for_exit "${spill_client}" 10 "spill"
wait_for_exit "${writer_client}" 10 "blocked writer"

final_count=$(sql -c "
    SELECT count(*)
    FROM (
        SELECT 1
        FROM spill_docs
        ORDER BY body <@> to_bm25query('alpha', 'spill_idx')
        LIMIT 501
    ) ranked;")
[ "${final_count}" = "501" ] ||
    fail "expected 501 ranked rows after spill, got ${final_count}"

sql -c "
    CREATE TABLE spill_cancel_docs (
        id integer PRIMARY KEY,
        body text NOT NULL
    );
    CREATE INDEX spill_cancel_idx ON spill_cancel_docs USING bm25(body)
        WITH (text_config = 'english', compaction = 'manual');
    INSERT INTO spill_cancel_docs
    SELECT gs, 'cancelterm document ' || gs
    FROM generate_series(1, 500) gs;" >/dev/null

if PGAPPNAME=spill-cancel sql -c "
    SELECT injection_points_set_local();
    SELECT injection_points_attach(
        '${POINT_SPILL_BEFORE_FINALIZE}', 'error');
    SELECT bm25_spill_index('spill_cancel_idx');" \
    >"${CLIENT_DIR}/cancel.log" 2>&1; then
    fail "injected spill error completed successfully"
fi

cancel_chain_count=$(sql -c "
    SELECT COALESCE(sum(n_records), 0)
    FROM bm25_memtable_chain('spill_cancel_idx');")
cancel_graph=$(sql -c "
    SELECT bm25_level_counts('spill_cancel_idx'::regclass)::text;")
cancel_ranked_count=$(sql -c "
    SELECT count(*)
    FROM (
        SELECT 1
        FROM spill_cancel_docs
        ORDER BY body <@> to_bm25query('cancelterm', 'spill_cancel_idx')
        LIMIT 500
    ) ranked;")
[ "${cancel_chain_count}" = "500" ] ||
    fail "failed spill state: chain=${cancel_chain_count} graph=${cancel_graph} ranked=${cancel_ranked_count}"
[ "${cancel_graph}" = "{0,0,0,0,0,0,0,0}" ] ||
    fail "failed spill published a segment"
[ "${cancel_ranked_count}" = "500" ] ||
    fail "failed spill made chain documents unqueryable"

sql -c "SELECT bm25_spill_index('spill_cancel_idx');" >/dev/null
cancel_graph=$(sql -c "
    SELECT bm25_level_counts('spill_cancel_idx'::regclass)::text;")
[ "${cancel_graph}" = "{1,0,0,0,0,0,0,0}" ] ||
    fail "spill after cancellation did not publish one segment"

sql -c "
    CREATE TABLE reclaim_docs (id integer PRIMARY KEY, body text NOT NULL);
    CREATE INDEX reclaim_idx ON reclaim_docs USING bm25(body)
        WITH (text_config = 'english', compaction = 'manual');" >/dev/null
for batch in 1 2; do
    sql -c "
        INSERT INTO reclaim_docs
        SELECT (${batch} - 1) * 200 + gs,
               'reclaimterm batch ${batch} document ' || gs
        FROM generate_series(1, 200) gs;
        SELECT bm25_spill_index('reclaim_idx');" >/dev/null
done
sql -c "SELECT bm25_compact_step('reclaim_idx'::regclass);" >/dev/null
[ "$(sql -c "SELECT bm25_pending_free_pages('reclaim_idx');")" -gt 0 ] ||
    fail "first compaction did not create deferred reclaim work"

for batch in 3 4; do
    sql -c "
        INSERT INTO reclaim_docs
        SELECT (${batch} - 1) * 200 + gs,
               'reclaimterm batch ${batch} document ' || gs
        FROM generate_series(1, 200) gs;
        SELECT bm25_spill_index('reclaim_idx');" >/dev/null
done

PGAPPNAME=reclaim-builder sql -c "
    SET statement_timeout = '60s';
    SELECT injection_points_set_local();
    SELECT injection_points_attach(
        '${POINT_TOMBSTONE_AFTER_UNLINK}', 'wait');
    SELECT bm25_compact_step('reclaim_idx'::regclass);" \
    >"${CLIENT_DIR}/reclaim.log" 2>&1 &
reclaim_client=$!
reclaim_backend=$(backend_pid reclaim-builder)

deadline=$((SECONDS + 10))
while ((SECONDS < deadline)); do
    if [ "$(sql -c "
        SELECT EXISTS (
            SELECT 1
            FROM pg_stat_activity
            WHERE pid = ${reclaim_backend}
              AND wait_event_type = 'InjectionPoint'
              AND wait_event = '${POINT_TOMBSTONE_AFTER_UNLINK}'
        );")" = "t" ]; then
        break
    fi
    sleep 0.05
done
reclaim_wait_event=$(sql -c "
    SELECT wait_event
    FROM pg_stat_activity
    WHERE pid = ${reclaim_backend};")
[ "${reclaim_wait_event}" = "${POINT_TOMBSTONE_AFTER_UNLINK}" ] ||
    fail "reclaim did not pause after tombstone unlink"

PGAPPNAME=reclaim-reader sql -c "
    SELECT count(*)
    FROM (
        SELECT 1
        FROM reclaim_docs
        ORDER BY body <@> to_bm25query('reclaimterm', 'reclaim_idx')
        LIMIT 800
    ) ranked;" >"${CLIENT_DIR}/reclaim_reader.log" 2>&1 &
reclaim_reader=$!
wait_for_exit "${reclaim_reader}" 3 "reader during tombstone free"
grep -qx '800' "${CLIENT_DIR}/reclaim_reader.log" ||
    fail "reader during tombstone free returned wrong results"

sql -c "SELECT injection_points_wakeup(
    '${POINT_TOMBSTONE_AFTER_UNLINK}');" >/dev/null
wait_for_exit "${reclaim_client}" 20 "reclaim compaction"

echo "nonblocking spill test passed"
