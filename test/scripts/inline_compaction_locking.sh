#!/bin/bash
#
# Regression tests for spill-time inline compaction admission.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55461
TEST_DB=inline_compaction_locking_test
DATA_DIR="${SCRIPT_DIR}/../tmp_inline_compaction_locking"
SOCKET_DIR="${TMPDIR:-/tmp}/pgts_inline_${TEST_PORT}"
LOGFILE="${DATA_DIR}/postgres.log"
CLIENT_DIR="${DATA_DIR}/clients"
TEST_CASE="${1:-all}"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
fail() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

cleanup() {
    local exit_code=$?

    jobs -p | xargs -r kill 2>/dev/null || true
    exec 3>&- 2>/dev/null || true
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate >/dev/null 2>&1 || true
    fi
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    exit "${exit_code}"
}

trap cleanup EXIT INT TERM

setup_test_db() {
    rm -rf "${DATA_DIR}" "${SOCKET_DIR}"
    mkdir -p "${DATA_DIR}" "${SOCKET_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1
    mkdir -p "${CLIENT_DIR}"
    cat >>"${DATA_DIR}/postgresql.conf" <<EOF
port = ${TEST_PORT}
unix_socket_directories = '${SOCKET_DIR}'
listen_addresses = 'localhost'
shared_preload_libraries = 'pg_textsearch'
autovacuum = off
deadlock_timeout = '100ms'
pg_textsearch.memtable_pages_threshold = 0
pg_textsearch.bulk_load_threshold = 0
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null
    createdb -h "${SOCKET_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

PSQL="psql -h ${SOCKET_DIR} -p ${TEST_PORT} -d ${TEST_DB} -qAt \
    -v ON_ERROR_STOP=1"

wait_for_file_text() {
    local file="$1"
    local text="$2"
    local description="$3"
    local deadline=$((SECONDS + 15))

    while ((SECONDS < deadline)); do
        if grep -Fq "${text}" "${file}" 2>/dev/null; then
            return
        fi
        sleep 0.1
    done

    tail -n 30 "${file}" 2>/dev/null || true
    fail "timed out waiting for ${description}"
}

wait_for_reindex_wait() {
    local application_name="$1"
    local index_name="$2"
    local deadline=$((SECONDS + 15))
    local waiting=0

    while ((SECONDS < deadline)); do
        waiting=$($PSQL -c "
            SELECT count(*)
            FROM pg_locks l
            JOIN pg_stat_activity a USING (pid)
            WHERE a.application_name = '${application_name}'
              AND l.relation = '${index_name}'::regclass
              AND l.mode = 'ShareUpdateExclusiveLock'
              AND l.granted
              AND a.wait_event_type = 'Lock';")
        if [ "${waiting:-0}" -eq 1 ]; then
            return
        fi
        sleep 0.1
    done

    fail "REINDEX CONCURRENTLY did not reach its writer wait"
}

test_reindex_deadlock() {
    local fifo="${CLIENT_DIR}/writer.fifo"
    local writer_log="${CLIENT_DIR}/writer.log"
    local reindex_log="${CLIENT_DIR}/reindex.log"
    local writer_pid
    local reindex_pid

    log "Testing DML spill against REINDEX CONCURRENTLY"
    $PSQL <<'SQL' >/dev/null
CREATE TABLE inline_deadlock (id integer PRIMARY KEY, body text NOT NULL);
INSERT INTO inline_deadlock VALUES (1, 'initial alpha document');
CREATE INDEX inline_deadlock_idx ON inline_deadlock USING bm25(body)
    WITH (text_config='english', compaction='inline');
SQL

    mkfifo "${fifo}"
    PGAPPNAME=pgts-inline-writer \
        $PSQL <"${fifo}" >"${writer_log}" 2>&1 &
    writer_pid=$!
    exec 3>"${fifo}"
    printf '%s\n' \
        "BEGIN;" \
        "SET LOCAL pg_textsearch.memtable_pages_threshold = 0;" \
        "UPDATE inline_deadlock SET body = body || ' first' WHERE id = 1;" \
        "SELECT 'writer-ready';" >&3
    wait_for_file_text "${writer_log}" "writer-ready" "writer setup"

    PGAPPNAME=pgts-inline-reindex \
        $PSQL -c "REINDEX INDEX CONCURRENTLY inline_deadlock_idx;" \
        >"${reindex_log}" 2>&1 &
    reindex_pid=$!
    wait_for_reindex_wait pgts-inline-reindex inline_deadlock_idx

    printf '%s\n' \
        "SET LOCAL pg_textsearch.memtable_pages_threshold = 1;" \
        "UPDATE inline_deadlock SET body = body || ' second' WHERE id = 1;" \
        "SELECT 'writer-finished';" \
        "COMMIT;" >&3
    exec 3>&-

    wait "${writer_pid}" || {
        cat "${writer_log}"
        cat "${reindex_log}"
        fail "writer aborted while REINDEX CONCURRENTLY held maintenance admission"
    }
    wait "${reindex_pid}" || {
        cat "${writer_log}"
        cat "${reindex_log}"
        fail "REINDEX CONCURRENTLY aborted during spill-time compaction"
    }

    grep -Fq "writer-finished" "${writer_log}" ||
        fail "writer did not complete its spill-triggering update"
    if grep -qi "deadlock detected" "${writer_log}" "${reindex_log}"; then
        fail "spill-time inline compaction deadlocked with REINDEX CONCURRENTLY"
    fi

    $PSQL -c "DROP TABLE inline_deadlock CASCADE;" >/dev/null
}

test_full_l0_defers_spill() {
    local chain_records
    local level_counts
    local ranked_count

    log "Testing automatic spill at a full but compactable L0"
    $PSQL <<'SQL' >/dev/null
CREATE TABLE inline_capacity (id integer PRIMARY KEY, body text NOT NULL);
CREATE INDEX inline_capacity_idx ON inline_capacity USING bm25(body)
    WITH (text_config='english', compaction='manual');
SET pg_textsearch.debug_segment_count_limit = 2;
SET pg_textsearch.segments_per_level = 2;
INSERT INTO inline_capacity VALUES (1, 'capacity alpha one');
SELECT bm25_spill_index('inline_capacity_idx');
INSERT INTO inline_capacity VALUES (2, 'capacity alpha two');
SELECT bm25_spill_index('inline_capacity_idx');
ALTER INDEX inline_capacity_idx SET (compaction='inline');
SET pg_textsearch.memtable_pages_threshold = 1;
INSERT INTO inline_capacity VALUES (3, 'capacity alpha three');
SQL

    chain_records=$($PSQL -c "
        SELECT COALESCE(sum(n_records), 0)
        FROM bm25_memtable_chain('inline_capacity_idx');")
    if [ "${chain_records:-0}" -lt 1 ]; then
        fail "full-L0 automatic spill did not preserve deferred memtable data"
    fi
    level_counts=$($PSQL -c "
        SELECT bm25_level_counts('inline_capacity_idx'::regclass);")
    if [ "${level_counts}" != "{0,1,0,0,0,0,0,0}" ]; then
        fail "full-L0 deferral did not run inline compaction policy"
    fi

    $PSQL -c "SELECT bm25_spill_index('inline_capacity_idx');" >/dev/null
    ranked_count=$($PSQL -c "
        SELECT count(*) FROM (
            SELECT 1
            FROM inline_capacity
            ORDER BY body <@> to_bm25query(
                'alpha', 'inline_capacity_idx')
        ) ranked;")
    if [ "${ranked_count}" -ne 3 ]; then
        fail "deferred full-L0 spill lost indexed documents"
    fi

    $PSQL <<'SQL' >/dev/null
RESET pg_textsearch.memtable_pages_threshold;
RESET pg_textsearch.segments_per_level;
RESET pg_textsearch.debug_segment_count_limit;
DROP TABLE inline_capacity CASCADE;
SQL
}

test_irreducible_full_l0_errors() {
    local error_log="${CLIENT_DIR}/irreducible.log"

    log "Testing an irreducible full L0 remains fail-closed"
    if $PSQL >"${error_log}" 2>&1 <<'SQL'
CREATE TABLE inline_irreducible (
    id integer PRIMARY KEY, body text NOT NULL);
CREATE INDEX inline_irreducible_idx ON inline_irreducible USING bm25(body)
    WITH (text_config='english', compaction='manual');
SET pg_textsearch.max_segment_size = '1MB';
SET pg_textsearch.debug_segment_count_limit = 2;
SET pg_textsearch.segments_per_level = 2;
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 0;
DO $$
DECLARE
    batch integer;
BEGIN
    FOR batch IN 0..1 LOOP
        INSERT INTO inline_irreducible
        SELECT batch * 2000 + gs,
               'common ' ||
                   repeat(md5((batch * 2000 + gs)::text), 32)
        FROM generate_series(1, 2000) gs;
        PERFORM bm25_spill_index('inline_irreducible_idx');
    END LOOP;
END
$$;
ALTER INDEX inline_irreducible_idx SET (compaction='inline');
SET pg_textsearch.memtable_pages_threshold = 1;
INSERT INTO inline_irreducible
VALUES (5000, 'irreducible capacity trigger');
SQL
    then
        fail "irreducible full L0 silently deferred the spill"
    fi
    if ! grep -Fq "bm25 segment count limit reached at level 0" \
        "${error_log}"; then
        cat "${error_log}"
        fail "irreducible full L0 failed for an unexpected reason"
    fi

    $PSQL -c "DROP TABLE inline_irreducible CASCADE;" >/dev/null
}

test_managed_reindex_admission() {
    local fifo="${CLIENT_DIR}/managed_holder.fifo"
    local holder_log="${CLIENT_DIR}/managed_holder.log"
    local managed_log="${CLIENT_DIR}/managed_step.log"
    local reindex_log="${CLIENT_DIR}/managed_reindex.log"
    local holder_pid
    local managed_pid
    local reindex_pid
    local deadline
    local target
    local target_index_oid
    local target_db_oid
    local target_spc_oid
    local target_relfilenumber
    local target_owner_oid

    log "Testing managed compaction declines REINDEX maintenance admission"
    $PSQL <<'SQL' >/dev/null
CREATE TABLE managed_reindex (
    id integer PRIMARY KEY, body text NOT NULL);
INSERT INTO managed_reindex VALUES (1, 'managed alpha document');
CREATE INDEX managed_reindex_idx ON managed_reindex USING bm25(body)
    WITH (text_config='english', compaction='manual');
UPDATE pg_catalog.pg_class
SET reloptions = reloptions || ARRAY['compaction=background']
WHERE oid = 'managed_reindex_idx'::regclass;
SQL
    target=$($PSQL -F '|' -c "
        SELECT c.oid, d.oid,
               coalesce(nullif(c.reltablespace, 0), d.dattablespace),
               pg_relation_filenode(c.oid), c.relowner
        FROM pg_class c
        JOIN pg_database d ON d.datname = current_database()
        WHERE c.oid = 'managed_reindex_idx'::regclass;")
    IFS='|' read -r target_index_oid target_db_oid target_spc_oid \
        target_relfilenumber target_owner_oid <<<"${target}"

    mkfifo "${fifo}"
    PGAPPNAME=pgts-managed-holder \
        $PSQL <"${fifo}" >"${holder_log}" 2>&1 &
    holder_pid=$!
    exec 3>"${fifo}"
    printf '%s\n' \
        "BEGIN;" \
        "UPDATE managed_reindex SET body = body || ' held' WHERE id = 1;" \
        "SELECT 'holder-ready';" >&3
    wait_for_file_text "${holder_log}" "holder-ready" "managed holder"

    PGAPPNAME=pgts-managed-reindex \
        $PSQL -c "REINDEX INDEX CONCURRENTLY managed_reindex_idx;" \
        >"${reindex_log}" 2>&1 &
    reindex_pid=$!
    wait_for_reindex_wait pgts-managed-reindex managed_reindex_idx

    PGAPPNAME=pgts-managed-step $PSQL -c "
        SELECT bm25_compact_step_if_current(
            ${target_index_oid}::oid, ${target_db_oid}::oid,
            ${target_spc_oid}::oid, ${target_relfilenumber}::oid,
            ${target_owner_oid}::oid);" >"${managed_log}" 2>&1 &
    managed_pid=$!
    deadline=$((SECONDS + 3))
    while kill -0 "${managed_pid}" 2>/dev/null && ((SECONDS < deadline)); do
        sleep 0.05
    done
    if kill -0 "${managed_pid}" 2>/dev/null; then
        fail "managed compaction blocked behind REINDEX maintenance admission"
    fi
    wait "${managed_pid}" || {
        cat "${managed_log}"
        fail "managed compaction errored while maintenance was unavailable"
    }
    if ! grep -Fxq "f" "${managed_log}"; then
        cat "${managed_log}"
        fail "managed compaction did not report deferred work"
    fi

    printf '%s\n' "COMMIT;" >&3
    exec 3>&-
    wait "${holder_pid}" || {
        cat "${holder_log}"
        fail "managed holder exited with an error"
    }
    wait "${reindex_pid}" || {
        cat "${reindex_log}"
        fail "managed REINDEX CONCURRENTLY exited with an error"
    }
    $PSQL -c "DROP TABLE managed_reindex CASCADE;" >/dev/null
}

setup_test_db
case "${TEST_CASE}" in
reindex)
    test_reindex_deadlock
    ;;
capacity)
    test_full_l0_defers_spill
    ;;
irreducible)
    test_irreducible_full_l0_errors
    ;;
managed)
    test_managed_reindex_admission
    ;;
all)
    test_reindex_deadlock
    test_full_l0_defers_spill
    test_irreducible_full_l0_errors
    test_managed_reindex_admission
    ;;
*)
    fail "unknown test case: ${TEST_CASE}"
    ;;
esac
log "Inline compaction locking tests passed"
