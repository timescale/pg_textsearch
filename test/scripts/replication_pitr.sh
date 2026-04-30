#!/bin/bash
#
# replication_pitr.sh — recovery-target-LSN test.
#
# Replays WAL up to a target LSN partway through an insert burst,
# then queries. Validates that bm25 index state at any LSN is
# coherent (not just at the tail).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55452
TEST_DB=replication_pitr
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_pitr_primary"
RECOVERED_DIR="${SCRIPT_DIR}/../tmp_repl_pitr_recovered"
ARCHIVE_DIR="${SCRIPT_DIR}/../tmp_repl_pitr_archive"

# Custom: only one node, but reuse logging/etc.
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }

pitr_cleanup() {
    for d in "${RECOVERED_DIR}" "${PRIMARY_DIR}"; do
        if [ -f "${d}/postmaster.pid" ]; then
            pg_ctl stop -D "${d}" -m fast 2>/dev/null \
                || pg_ctl stop -D "${d}" -m immediate 2>/dev/null \
                || true
        fi
    done
    rm -rf "${PRIMARY_DIR}" "${RECOVERED_DIR}" "${ARCHIVE_DIR}"
}
if [ "${PITR_KEEP:-0}" != "1" ]; then
    trap pitr_cleanup EXIT INT TERM
fi

setup_archive_primary() {
    log "Setting up archive-mode primary on port ${PRIMARY_PORT}..."
    rm -rf "${PRIMARY_DIR}" "${ARCHIVE_DIR}"
    mkdir -p "${PRIMARY_DIR}" "${ARCHIVE_DIR}"
    initdb -D "${PRIMARY_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1
    cat >> "${PRIMARY_DIR}/postgresql.conf" <<EOF
port = ${PRIMARY_PORT}
shared_buffers = 128MB
log_min_messages = notice
logging_collector = on
log_filename = 'postgres.log'
shared_preload_libraries = 'pg_textsearch'
wal_level = replica
archive_mode = on
archive_command = 'cp %p ${ARCHIVE_DIR}/%f'
EOF
    pg_ctl start -D "${PRIMARY_DIR}" \
        -l "${PRIMARY_DIR}/postgres.log" -w
    createdb -p "${PRIMARY_PORT}" "${TEST_DB}"
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

# -------------------------------------------------------------------
# PITR-1: Recover to LSN partway through insert burst.
# Replays WAL up to a target LSN partway through an insert burst,
# then queries the recovered cluster.
#
# Expected: FAIL today — exposes a third bug class beyond the
# memtable-staleness and WAL-replayed-segment-unreadable bugs.
# pg_textsearch's WAL replay path rebuilds memtable posting entries
# but does NOT restore index-level corpus statistics (total_docs,
# total_len). With those counters at 0, BM25 IDF and BMW pivot
# selection produce 0 results even though the heap and memtable are
# correctly populated. A REINDEX rebuilds the counters and produces
# a working index. Will pass once WAL records cover the corpus
# statistics, or once the rmgr-based replication implementation
# rebuilds them on first scan.
# -------------------------------------------------------------------
test_recover_to_intermediate_lsn() {
    log "=== PITR-1: recover to mid-burst LSN ==="

    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -c "
        DROP TABLE IF EXISTS p1_docs CASCADE;
        CREATE TABLE p1_docs (id SERIAL PRIMARY KEY, content TEXT);
        CREATE INDEX p1_idx ON p1_docs USING bm25(content)
            WITH (text_config='simple');
        SELECT pg_switch_wal();
    " >/dev/null

    # Take basebackup BEFORE the inserts. PITR replays WAL forward
    # from the backup's consistent recovery point, so the target LSN
    # must be >= that point. Taking the backup here keeps the
    # consistent recovery point below the first batch of inserts.
    rm -rf "${RECOVERED_DIR}"
    pg_basebackup -D "${RECOVERED_DIR}" -p "${PRIMARY_PORT}" \
        -X stream --checkpoint=fast >/dev/null 2>&1

    # First half of inserts.
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -c "
        INSERT INTO p1_docs (content)
            SELECT 'first ' || i || ' alpha'
            FROM generate_series(1,200) i;
        SELECT pg_switch_wal();
    " >/dev/null

    # Capture the target LSN here.
    local target_lsn
    target_lsn=$(psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c "
        SELECT pg_current_wal_lsn();")
    log "  target LSN: ${target_lsn}"

    # Second half of inserts (these should NOT be visible in the
    # PITR-recovered cluster).
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -c "
        INSERT INTO p1_docs (content)
            SELECT 'second ' || i || ' alpha'
            FROM generate_series(1,200) i;
        SELECT pg_switch_wal();
    " >/dev/null

    # Force archive of the WAL containing target_lsn.
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -c "
        SELECT pg_switch_wal();
        CHECKPOINT;
    " >/dev/null
    sleep 2

    # Configure the recovered cluster for PITR.
    cat >> "${RECOVERED_DIR}/postgresql.conf" <<EOF
port = $((PRIMARY_PORT + 1))
restore_command = 'cp ${ARCHIVE_DIR}/%f %p'
recovery_target_lsn = '${target_lsn}'
recovery_target_action = 'pause'
EOF
    touch "${RECOVERED_DIR}/recovery.signal"

    pg_ctl start -D "${RECOVERED_DIR}" \
        -l "${RECOVERED_DIR}/postgres.log" -w

    # Wait for it to reach the target and pause.
    local i=0
    while [ $i -lt 20 ]; do
        local paused
        paused=$(psql -p "$((PRIMARY_PORT + 1))" -d "${TEST_DB}" -tA \
            -c "SELECT pg_get_wal_replay_pause_state();" 2>/dev/null)
        if [ "${paused}" = "paused" ]; then break; fi
        sleep 1; i=$((i + 1))
    done

    # Promote (resume) so we can query.
    psql -p "$((PRIMARY_PORT + 1))" -d "${TEST_DB}" -c "
        SELECT pg_wal_replay_resume();
    " >/dev/null
    pg_ctl promote -D "${RECOVERED_DIR}" -w
    sleep 2

    local count
    count=$(psql -p "$((PRIMARY_PORT + 1))" -d "${TEST_DB}" -tA -c "
        SELECT count(*) FROM (SELECT id FROM p1_docs
            ORDER BY content <@> to_bm25query('alpha','p1_idx')
            LIMIT 1000) t;" 2>/dev/null)
    log "  PITR-recovered count: ${count} (expect 200, was 400 \
before recovery cutoff)"

    if [[ "${count}" == *ERROR* ]] || [ "${count}" != "200" ]; then
        error "PITR-1 BUG (expected): recovered count=${count}, \
expected 200. Likely cause: corpus stats (total_docs, total_len) \
not replayed via WAL — BM25 IDF returns 0 results."
    fi
    log "PITR-1 PASSED"
}

main() {
    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"
    command -v pg_basebackup >/dev/null 2>&1 || \
        error "pg_basebackup not found"
    command -v initdb >/dev/null 2>&1 || error "initdb not found"

    setup_archive_primary
    test_recover_to_intermediate_lsn

    log "All PITR tests completed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
