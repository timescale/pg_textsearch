#!/bin/bash
#
# Hold an active ranked cursor on a hot standby's old segment graph while
# the primary publishes a replacement graph.  hot_standby_feedback must
# keep the displaced source pages parked until the cursor and snapshot end.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55438
STANDBY_PORT=55439
TEST_DB=standby_reclaim_test
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_standby_reclaim_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_standby_reclaim_standby"

# Avoid Unix-socket path limits in deeply nested worktrees.
REPL_HOST=127.0.0.1
REPL_SOCKET_DIR=

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

READER_PID=
READER_BACKEND_PID=
READER_OPEN=false
HELD_FEEDBACK_XMIN=

wait_for_child_exit() {
    local pid=$1
    local attempts=$2

    for _ in $(seq 1 "${attempts}"); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            # The bounded poll established that the child exited; this wait
            # only reaps its already-available status.
            wait "${pid}" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    return 1
}

reader_close() {
    if [ "${READER_OPEN}" != "true" ]; then
        return
    fi

    exec 9>&- || true
    exec 8<&- || true
    if ! wait_for_child_exit "${READER_PID}" 50; then
        warn "Ranked reader PID ${READER_PID} did not exit after EOF in 5s; \
sending SIGTERM"
        kill -TERM "${READER_PID}" 2>/dev/null || true
        if ! wait_for_child_exit "${READER_PID}" 50; then
            warn "Ranked reader PID ${READER_PID} ignored SIGTERM for 5s; \
sending SIGKILL (state: $(ps -o pid=,stat=,cmd= -p "${READER_PID}" \
2>/dev/null || echo unavailable))"
            kill -9 "${READER_PID}" 2>/dev/null || true
            if ! wait_for_child_exit "${READER_PID}" 50; then
                warn "Ranked reader PID ${READER_PID} still exists 5s after \
SIGKILL (state: $(ps -o pid=,stat=,cmd= -p "${READER_PID}" \
2>/dev/null || echo unavailable)); continuing bounded cleanup"
            fi
        fi
    fi
    READER_OPEN=false
    rm -rf "${STANDBY_DIR}/ranked_reader"
}

cleanup() {
    local status=$?

    set +e
    reader_close
    if [ "${status}" -eq 0 ]; then
        true
    else
        (exit "${status}")
    fi
    repl_cleanup
}
trap cleanup EXIT INT TERM

reader_open() {
    local control_dir="${STANDBY_DIR}/ranked_reader"

    rm -rf "${control_dir}"
    mkdir -p "${control_dir}"
    mkfifo "${control_dir}/in" "${control_dir}/out"

    psql -X -tAq -v ON_ERROR_STOP=1 -p "${STANDBY_PORT}" \
        -d "${TEST_DB}" \
        < "${control_dir}/in" > "${control_dir}/out" 2>&1 &
    READER_PID=$!
    exec 9>"${control_dir}/in"
    exec 8<"${control_dir}/out"
    READER_OPEN=true
}

reader_query() {
    local sql=$1
    local sentinel="__standby_reclaim_$$_${RANDOM}__"
    local result=""
    local line

    printf '%s\n' "${sql}" >&9
    printf "SELECT '%s';\n" "${sentinel}" >&9

    while true; do
        if ! IFS= read -r -t 30 line <&8; then
            error "Timed out 30s waiting for ranked reader PID ${READER_PID}; \
backend=${READER_BACKEND_PID:-unknown}, partial output=${result:-none}"
        fi
        if [ "${line}" = "${sentinel}" ]; then
            break
        fi
        if [ -n "${result}" ]; then
            result="${result}"$'\n'"${line}"
        else
            result="${line}"
        fi
    done
    printf '%s' "${result}"
}

wait_for_feedback_xmin() {
    local xmin=""

    for _ in $(seq 1 60); do
        xmin=$(primary_sql_quiet "
            SELECT backend_xmin
              FROM pg_stat_replication
             WHERE state = 'streaming'
               AND backend_xmin IS NOT NULL
             LIMIT 1;")
        if [ -n "${xmin}" ]; then
            HELD_FEEDBACK_XMIN="${xmin}"
            log "Primary sees standby feedback xmin ${xmin}"
            return 0
        fi
        sleep 0.5
    done

    error "Timed out 30s waiting for hot-standby feedback; replication state: \
$(primary_sql_quiet "
    SELECT application_name, state, backend_xmin, replay_lsn
      FROM pg_stat_replication;")"
}

wait_for_feedback_release() {
    local xmin=""

    for _ in $(seq 1 60); do
        xmin=$(primary_sql_quiet "
            SELECT coalesce(backend_xmin::text, '')
              FROM pg_stat_replication
             WHERE state = 'streaming'
             LIMIT 1;")
        if [ -z "${xmin}" ] ||
           [ "${xmin}" -gt "${HELD_FEEDBACK_XMIN}" ]; then
            log "Primary feedback xmin advanced from \
${HELD_FEEDBACK_XMIN} to ${xmin:-none}"
            return 0
        fi
        sleep 0.5
    done

    error "Timed out 30s waiting for standby xmin to advance beyond \
${HELD_FEEDBACK_XMIN}; backend_xmin=${xmin}"
}

assert_ranked_plan() {
    local port=$1
    local plan

    plan=$(node_sql_quiet "${port}" "
        SET enable_seqscan = off;
        EXPLAIN (COSTS off)
        SELECT id
          FROM rec
         ORDER BY body <@> to_bm25query('alpha', 'rec_idx')
         LIMIT 8000;")
    grep -Fq "Index Scan using rec_idx on rec" <<<"${plan}" ||
        error "Port ${port} ranked query did not use rec_idx: ${plan}"
}

main() {
    local spilled graph feedback_setting plan first_id remaining
    local parked_before_vacuum parked_after_vacuum drained
    local expected_ids primary_ids standby_ids
    local held_ids held_ids_sorted held_id_count duplicate_ids

    check_required_tools
    setup_primary

    primary_sql "
        CREATE TABLE rec (
            id integer PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        INSERT INTO rec
        SELECT g, 'alpha beta gamma standby document ' || g
          FROM generate_series(1, 4000) g;
        CREATE INDEX rec_idx ON rec USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');
        INSERT INTO rec
        SELECT g, 'alpha beta gamma standby document ' || g
          FROM generate_series(4001, 8000) g;
    " >/dev/null
    spilled=$(primary_sql_quiet "SELECT bm25_spill_index('rec_idx') > 0;")
    [ "${spilled}" = "t" ] ||
        error "Second L0 segment was not spilled (got '${spilled}')"
    graph=$(primary_sql_quiet \
        "SELECT bm25_level_counts('rec_idx'::regclass)::text;")
    [ "${graph}" = "{2,0,0,0,0,0,0,0}" ] ||
        error "Expected two-L0 old graph, got ${graph}"

    setup_standby
    cat >> "${STANDBY_DIR}/postgresql.conf" <<EOF
hot_standby_feedback = on
wal_receiver_status_interval = 1s
EOF
    pg_ctl restart -D "${STANDBY_DIR}" \
        -l "${STANDBY_DIR}/postgres.log" -w -t 30 >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up before ranked cursor setup"
    feedback_setting=$(standby_sql_quiet "SHOW hot_standby_feedback;")
    [ "${feedback_setting}" = "on" ] ||
        error "hot_standby_feedback is ${feedback_setting}, expected on"
    assert_ranked_plan "${STANDBY_PORT}"

    reader_open
    READER_BACKEND_PID=$(reader_query "SELECT pg_backend_pid();")
    reader_query "BEGIN ISOLATION LEVEL REPEATABLE READ;" >/dev/null
    reader_query "SET enable_seqscan = off;" >/dev/null
    plan=$(reader_query "
        EXPLAIN (COSTS off)
        SELECT id
          FROM rec
         ORDER BY body <@> to_bm25query('alpha', 'rec_idx')
         LIMIT 8000;")
    grep -Fq "Index Scan using rec_idx on rec" <<<"${plan}" ||
        error "Held standby cursor would not use rec_idx: ${plan}"
    reader_query "
        DECLARE held_ranked NO SCROLL CURSOR FOR
        SELECT id
          FROM rec
         ORDER BY body <@> to_bm25query('alpha', 'rec_idx')
         LIMIT 8000;" >/dev/null
    first_id=$(reader_query "FETCH FORWARD 1 FROM held_ranked;")
    [ -n "${first_id}" ] ||
        error "Held ranked cursor returned no first document"
    wait_for_feedback_xmin
    log "Ranked cursor PID ${READER_BACKEND_PID} is open on the old graph"

    primary_sql "SELECT bm25_force_merge('rec_idx');" >/dev/null
    parked_before_vacuum=$(primary_sql_quiet \
        "SELECT bm25_pending_free_pages('rec_idx');")
    case "${parked_before_vacuum}" in
        ''|*[!0-9]*) error "Invalid parked count '${parked_before_vacuum}'" ;;
    esac
    [ "${parked_before_vacuum}" -gt 0 ] ||
        error "Compaction published without parking displaced pages"

    # Assign and commit an xid so synchronous commit flushes the publication
    # WAL, then make a reclaim attempt while feedback pins the old snapshot.
    primary_sql "CREATE TABLE reclaim_flush_marker (id integer);" >/dev/null
    primary_sql "VACUUM rec;" >/dev/null
    parked_after_vacuum=$(primary_sql_quiet \
        "SELECT bm25_pending_free_pages('rec_idx');")
    [ "${parked_after_vacuum}" = "${parked_before_vacuum}" ] ||
        error "VACUUM reclaimed old-graph pages while ranked cursor was open: \
before=${parked_before_vacuum} after=${parked_after_vacuum}"

    wait_for_standby_catchup 30 ||
        error "Standby did not replay compaction publication"
    remaining=$(reader_query "FETCH ALL FROM held_ranked;")
    held_ids="${first_id}"$'\n'"${remaining}"
    if grep -Ev '^[0-9]+$' <<<"${held_ids}" >/dev/null; then
        error "Old-layout ranked cursor returned a non-numeric document ID"
    fi
    held_id_count=$(printf '%s\n' "${held_ids}" | wc -l | tr -d ' ')
    held_ids_sorted=$(printf '%s\n' "${held_ids}" |
        LC_ALL=C sort -n | paste -sd, -)
    expected_ids=$(seq 1 8000 | paste -sd, -)
    if [ "${held_ids_sorted}" != "${expected_ids}" ]; then
        duplicate_ids=$(printf '%s\n' "${held_ids}" |
            LC_ALL=C sort -n | uniq -d | head -10 | paste -sd, -)
        error "Old-layout ranked cursor returned the wrong exact ID set: \
count=${held_id_count}, duplicate sample=${duplicate_ids:-none}"
    fi
    log "PASS: old-layout ranked cursor returned exact IDs 1..8000 \
after publication replay"

    reader_query "CLOSE held_ranked; COMMIT;" >/dev/null
    reader_close
    wait_for_feedback_release

    drained="${parked_after_vacuum}"
    for _ in $(seq 1 20); do
        primary_sql_quiet "SELECT txid_current();" >/dev/null
        primary_sql_quiet "SELECT txid_current();" >/dev/null
        primary_sql "VACUUM rec;" >/dev/null
        drained=$(primary_sql_quiet \
            "SELECT bm25_pending_free_pages('rec_idx');")
        [ "${drained}" = "0" ] && break
        sleep 0.5
    done
    [ "${drained}" = "0" ] ||
        error "Timed out 10s draining parked pages after snapshot release; \
remaining=${drained}, backend_xmin=$(primary_sql_quiet "
SELECT coalesce(backend_xmin::text, '') FROM pg_stat_replication LIMIT 1;")"

    wait_for_standby_catchup 30 ||
        error "Standby did not catch up after safe reclaim"
    assert_ranked_plan "${PRIMARY_PORT}"
    assert_ranked_plan "${STANDBY_PORT}"
    expected_ids=$(primary_sql_quiet \
        "SELECT string_agg(id::text, ',' ORDER BY id) FROM rec;")
    primary_ids=$(primary_sql_quiet "
        SET enable_seqscan = off;
        SELECT string_agg(id::text, ',' ORDER BY id)
          FROM (
                SELECT id FROM rec
                 ORDER BY body <@> to_bm25query('alpha', 'rec_idx')
                 LIMIT 8000
               ) ranked;" | tail -n 1)
    standby_ids=$(standby_sql_quiet "
        SET enable_seqscan = off;
        SELECT string_agg(id::text, ',' ORDER BY id)
          FROM (
                SELECT id FROM rec
                 ORDER BY body <@> to_bm25query('alpha', 'rec_idx')
                 LIMIT 8000
               ) ranked;" | tail -n 1)
    [ "${primary_ids}" = "${expected_ids}" ] ||
        error "Primary ranked IDs differ from exact heap IDs after reclaim"
    [ "${standby_ids}" = "${expected_ids}" ] ||
        error "Standby ranked IDs differ from exact heap IDs after reclaim"

    log "PASS: VACUUM reclaimed ${parked_before_vacuum} pages only after \
the standby cursor ended"
    log "All standby reclaim overlap checks passed"
}

main "$@"
