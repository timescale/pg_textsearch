#!/bin/bash
#
# replication_segment_reclaim.sh -- acceptance test for the deferred,
# standby-safe reclaim of displaced segment pages (issue #380).
#
# Scenario (the two #380 acceptance criteria):
#   1. A standby holding an open snapshot, with hot_standby_feedback=on,
#      holds back the primary's reclaim horizon.  A merge on the primary
#      PARKS the displaced segment pages; a following VACUUM must NOT
#      recycle them while the standby snapshot is live -- so
#      bm25_pending_free_pages stays > 0 and the standby keeps answering
#      queries correctly (no "could not read block", no corruption).
#   2. Once the standby snapshot is released and the horizon advances,
#      a VACUUM on the primary drains every parked page to the FSM
#      (bm25_pending_free_pages drops to 0).
#
# Uses the shared replication harness (replication_lib.sh).  Asserts
# explicitly; repl_cleanup preserves the failing exit status.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55438
STANDBY_PORT=55439
TEST_DB=segment_reclaim_repl_test
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_reclaim_repl_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_reclaim_repl_standby"

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

# Wait until the primary sees the standby's feedback xmin (proves
# hot_standby_feedback is propagating and will hold back the horizon).
wait_for_backend_xmin() {
    local i=0
    while [ $i -lt 20 ]; do
        local x
        x=$(primary_sql_quiet "SELECT backend_xmin FROM pg_stat_replication WHERE backend_xmin IS NOT NULL LIMIT 1;")
        if [ -n "$x" ]; then
            log "Primary sees standby backend_xmin=${x}"
            return 0
        fi
        sleep 0.5
        i=$((i + 1))
    done
    return 1
}

main() {
    check_required_tools

    setup_primary

    # Build a two-L0-segment index: CREATE INDEX writes one segment
    # directly; a second batch fills the memtable; a spill writes the
    # second segment.  A later force-merge will displace both.
    log "Loading data and spilling a second segment on the primary..."
    primary_sql "CREATE TABLE rec (id int, body text) WITH (autovacuum_enabled = false);" >/dev/null
    primary_sql "INSERT INTO rec SELECT g, 'alpha beta gamma delta term' || (g % 50) FROM generate_series(1, 4000) g;" >/dev/null
    primary_sql "CREATE INDEX rec_idx ON rec USING bm25 (body) WITH (text_config = 'english');" >/dev/null
    primary_sql "INSERT INTO rec SELECT g, 'alpha beta term' || (g % 50) FROM generate_series(4001, 8000) g;" >/dev/null
    spilled=$(primary_sql_quiet "SELECT bm25_spill_index('rec_idx') > 0;")
    [ "$spilled" = "t" ] || error "spill did not write a segment (got '$spilled')"

    # Bring up the standby (basebackup snapshots the two L0 segments),
    # with hot_standby_feedback on and frequent status updates so the
    # standby's snapshot xmin reaches the primary quickly.
    setup_standby
    cat >> "${STANDBY_DIR}/postgresql.conf" <<EOF
hot_standby_feedback = on
wal_receiver_status_interval = 1s
EOF
    pg_ctl restart -D "${STANDBY_DIR}" -l "${STANDBY_DIR}/postgres.log" -w >/dev/null
    wait_for_standby_catchup

    # Sanity: the standby answers the index query before the merge.
    pre=$(search_count "${STANDBY_PORT}" rec body "alpha" "rec_idx")
    [ -n "$pre" ] && [ "$pre" -gt 0 ] || error "standby query returned no rows pre-merge (got '$pre')"
    log "Standby answered pre-merge query: ${pre} rows."

    # Open a long-lived REPEATABLE READ snapshot on the standby and hold
    # it open across the merge.  This is what pins the primary's horizon.
    long_lived_open "${STANDBY_PORT}"
    long_lived_query "BEGIN ISOLATION LEVEL REPEATABLE READ;" >/dev/null
    held=$(long_lived_query "SELECT count(*) FROM (SELECT id FROM rec ORDER BY body <@> to_bm25query('alpha', 'rec_idx') LIMIT 50) s;")
    [ -n "$held" ] && [ "$held" -gt 0 ] || error "standby long-lived query returned no rows (got '$held')"
    log "Standby holds a REPEATABLE READ snapshot (query saw ${held} rows)."

    wait_for_backend_xmin || error "primary never saw standby backend_xmin (hot_standby_feedback not propagating)"

    # Merge on the primary parks the displaced pages; force the merge's
    # WAL to disk (force-merge assigns no xid) so it streams to the
    # standby; then VACUUM attempts to drain.  The live standby snapshot
    # must hold the horizon back, so the parked pages stay parked.
    log "Forcing merge + VACUUM on the primary (standby snapshot live)..."
    primary_sql "SELECT bm25_force_merge('rec_idx');" >/dev/null
    primary_sql "CREATE TABLE flush_marker (x int);" >/dev/null
    primary_sql "VACUUM rec;" >/dev/null

    parked_held=$(primary_sql_quiet "SELECT bm25_pending_free_pages('rec_idx');")
    case "$parked_held" in
        ''|*[!0-9]*) error "pending_free_pages not an integer: '$parked_held'" ;;
    esac
    [ "$parked_held" -gt 0 ] || error "parked pages were recycled while standby snapshot was live (got $parked_held, expected > 0)"
    log "PASS: $parked_held page(s) stay parked while standby snapshot is live."

    # The standby must replay the merge and keep answering correctly --
    # the displaced pages it may still reference were NOT recycled.
    wait_for_standby_catchup
    post=$(search_count "${STANDBY_PORT}" rec body "alpha" "rec_idx")
    [ -n "$post" ] && [ "$post" -gt 0 ] || error "standby query failed/empty after merge replay (got '$post')"
    log "PASS: standby answers correctly after merge replay (${post} rows)."

    # The held snapshot itself still reads cleanly (no torn pages).
    still=$(long_lived_query "SELECT count(*) FROM (SELECT id FROM rec ORDER BY body <@> to_bm25query('beta', 'rec_idx') LIMIT 50) s;")
    [[ "$still" == *ERROR* ]] && error "standby snapshot query errored after merge: ${still}"
    [ -n "$still" ] && [ "$still" -gt 0 ] || error "standby snapshot query returned no rows after merge (got '$still')"
    log "PASS: held standby snapshot still reads cleanly (${still} rows)."

    # Release the snapshot; the primary's horizon can now advance.
    long_lived_query "COMMIT;" >/dev/null
    long_lived_close

    # Advance the xid horizon past the merge stamp and VACUUM again;
    # every parked page must now be reclaimed.  Retry to absorb feedback
    # propagation lag after the standby releases its xmin.
    log "Releasing snapshot; draining on the primary..."
    drained=""
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        primary_sql_quiet "SELECT txid_current();" >/dev/null
        primary_sql_quiet "SELECT txid_current();" >/dev/null
        primary_sql "VACUUM rec;" >/dev/null
        drained=$(primary_sql_quiet "SELECT bm25_pending_free_pages('rec_idx');")
        [ "$drained" = "0" ] && break
        sleep 0.5
    done
    [ "$drained" = "0" ] || error "VACUUM did not drain parked pages after snapshot release (got '$drained', expected 0)"
    log "PASS: all parked pages reclaimed after snapshot release (now 0)."

    # Both nodes still healthy.
    wait_for_standby_catchup
    pcnt=$(search_count "${PRIMARY_PORT}" rec body "gamma" "rec_idx")
    scnt=$(search_count "${STANDBY_PORT}" rec body "gamma" "rec_idx")
    [ "$pcnt" = "$scnt" ] || error "primary/standby result mismatch after reclaim: primary=$pcnt standby=$scnt"
    log "PASS: primary and standby agree after reclaim ($pcnt rows)."

    log "✅ All standby segment-reclaim acceptance checks passed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
