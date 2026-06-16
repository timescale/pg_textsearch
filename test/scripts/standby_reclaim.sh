#!/bin/bash
#
# Crash-recovery coverage for the deferred, standby-safe segment reclaim
# (issue #380).  A merge PARKS displaced pages in the on-disk tombstone
# chain (WAL-logged via GenericXLog) instead of freeing them.  This
# script verifies that:
#
#   1. parked pages survive a crash (immediate stop) and WAL replay --
#      the tombstone chain and metapage pending_free_head are
#      reconstructed by stock PostgreSQL recovery (no custom rmgr);
#   2. the index still answers top-k queries after recovery;
#   3. bm25_pending_free_pages stays finite/consistent across the crash;
#   4. once the global xid horizon advances past the merge stamp, a
#      VACUUM drains every parked page back to the FSM.
#
# NOTE: recovery.sh's cleanup trap exit 0's, masking failures.  This
# script deliberately does NOT do that: cleanup preserves the failing
# exit status, and every check is an explicit assertion.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT=55436
TEST_DB=reclaim_recovery_test
DATA_DIR="${SCRIPT_DIR}/../tmp_reclaim_test"
LOGFILE="${DATA_DIR}/postgres.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

log()  { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
fail() { echo -e "${RED}[$(date '+%H:%M:%S')] FAIL: $1${NC}"; exit 1; }

cleanup() {
    local status=$?
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    # Preserve the real exit status -- do NOT mask failures with exit 0.
    exit $status
}
trap cleanup EXIT INT TERM

psql_run() { psql -tA -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" 2>/dev/null; }

setup() {
    log "initdb scratch cluster..."
    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"
    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
unix_socket_directories = '${DATA_DIR}'
shared_buffers = 128MB
max_connections = 20
shared_preload_libraries = 'pg_textsearch'
hot_standby_feedback = on
autovacuum = off
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null
    createdb -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql_run "CREATE EXTENSION pg_textsearch;" >/dev/null
}

restart() {
    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null
    psql_run "SELECT 1;" >/dev/null || fail "server did not come back after crash"
}

crash() {
    # -m immediate == no clean shutdown == crash; recovery replays WAL.
    pg_ctl stop -D "${DATA_DIR}" -m immediate -w >/dev/null 2>&1 || true
}

main() {
    command -v initdb >/dev/null 2>&1 || fail "initdb not found in PATH"

    setup

    # Build a state that parks pages: CREATE INDEX writes a segment
    # directly; a second batch fills the memtable; a spill produces a
    # second segment; force-merge compacts the two L0 segments into one
    # L1 segment, PARKING the displaced source pages.
    log "Loading data and forcing a merge to park displaced pages..."
    psql_run "CREATE TABLE rec (id int, body text) WITH (autovacuum_enabled = false);" >/dev/null
    psql_run "INSERT INTO rec SELECT g, 'alpha beta gamma delta term' || (g % 50) FROM generate_series(1, 4000) g;" >/dev/null
    psql_run "CREATE INDEX rec_idx ON rec USING bm25 (body) WITH (text_config = 'english');" >/dev/null
    psql_run "INSERT INTO rec SELECT g, 'alpha beta term' || (g % 50) FROM generate_series(4001, 8000) g;" >/dev/null

    spilled=$(psql_run "SELECT bm25_spill_index('rec_idx') > 0;")
    [ "$spilled" = "t" ] || fail "spill did not write a segment (got '$spilled')"

    psql_run "SELECT bm25_force_merge('rec_idx');" >/dev/null

    parked_before=$(psql_run "SELECT bm25_pending_free_pages('rec_idx');")
    case "$parked_before" in
        ''|*[!0-9]*) fail "pending_free_pages not an integer before crash: '$parked_before'" ;;
    esac
    [ "$parked_before" -gt 0 ] || fail "merge parked no pages (got $parked_before)"
    log "Merge parked $parked_before displaced page(s)."

    # bm25_force_merge writes its changes only through GenericXLog and
    # assigns no xid, so the implicit transaction commits WITHOUT a
    # commit record and never forces an XLogFlush.  An immediate crash
    # here would atomically roll the whole merge back (parking included)
    # -- consistent, but it would not exercise replay.  Force the merge
    # records to disk with a committing, xid-assigning transaction (NOT
    # a checkpoint, which would flush the data pages too and leave
    # nothing for redo) so the crash below makes recovery REDO the
    # tombstone enqueue and the metapage swap -- the same path a hot
    # standby takes.
    psql_run "CREATE TABLE flush_marker (x int);" >/dev/null

    # Crash BEFORE any drain so the parked tombstone chain must be
    # rebuilt by WAL replay alone.
    log "Crashing server (immediate stop) with pages still parked..."
    crash
    log "Restarting -- recovery must REDO the tombstone chain..."
    restart

    # (1) pending_free_pages must not error and must match the count
    #     that was durable before the crash (WAL replay reconstructed it).
    parked_after_crash=$(psql_run "SELECT bm25_pending_free_pages('rec_idx');")
    case "$parked_after_crash" in
        ''|*[!0-9]*) fail "pending_free_pages not an integer after recovery: '$parked_after_crash'" ;;
    esac
    [ "$parked_after_crash" = "$parked_before" ] || \
        fail "parked count changed across crash: before=$parked_before after=$parked_after_crash"
    log "PASS: $parked_after_crash parked page(s) survived crash recovery."

    # (2) the index still answers top-k queries after recovery.
    hits=$(psql_run "SELECT count(*) FROM (SELECT 1 FROM rec ORDER BY body <@> to_bm25query('alpha', 'rec_idx') LIMIT 20) s;")
    [ -n "$hits" ] && [ "$hits" -gt 0 ] || fail "post-recovery top-k query returned no rows (got '$hits')"
    log "PASS: post-recovery top-k query returned $hits row(s)."

    # (3) advance the global xid horizon past the merge stamp, then a
    #     VACUUM must drain every parked page.
    psql_run "SELECT txid_current();" >/dev/null
    psql_run "SELECT txid_current();" >/dev/null
    psql_run "VACUUM rec;" >/dev/null

    parked_after_vacuum=$(psql_run "SELECT bm25_pending_free_pages('rec_idx');")
    [ "$parked_after_vacuum" = "0" ] || \
        fail "VACUUM did not drain parked pages (got '$parked_after_vacuum', expected 0)"
    log "PASS: VACUUM drained all parked pages (now 0)."

    # (4) index is still healthy after the drain.
    hits=$(psql_run "SELECT count(*) FROM (SELECT 1 FROM rec ORDER BY body <@> to_bm25query('beta', 'rec_idx') LIMIT 20) s;")
    [ -n "$hits" ] && [ "$hits" -gt 0 ] || fail "post-drain query returned no rows (got '$hits')"
    log "PASS: index healthy after reclaim ($hits row(s))."

    log "✅ All tombstone crash-recovery checks passed!"
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
