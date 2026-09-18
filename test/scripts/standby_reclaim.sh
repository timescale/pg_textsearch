#!/bin/bash
#
# Start a ranked cursor on a hot standby's old segment graph after compaction
# has finished its unlocked build but before publication.
# hot_standby_feedback must keep the displaced source pages parked until the
# cursor and snapshot end. A second case disconnects the standby and verifies
# that stock recovery-conflict WAL cancels the old reader before page reuse.

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
COMPACTOR_PID=
SNAPSHOT_READER_PID=

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
    if [ -n "${SNAPSHOT_READER_PID}" ] &&
       kill -0 "${SNAPSHOT_READER_PID}" 2>/dev/null; then
        kill -TERM "${SNAPSHOT_READER_PID}" 2>/dev/null || true
        wait_for_child_exit "${SNAPSHOT_READER_PID}" 50 || true
    fi
    if [ -n "${COMPACTOR_PID}" ] &&
       kill -0 "${COMPACTOR_PID}" 2>/dev/null; then
        kill -TERM "${COMPACTOR_PID}" 2>/dev/null || true
        wait_for_child_exit "${COMPACTOR_PID}" 50 || true
    fi
    reader_close
    if [ "${status}" -eq 0 ]; then
        true
    else
        (exit "${status}")
    fi
    repl_cleanup
}
trap cleanup EXIT INT TERM

wait_for_compaction_pause() {
    local index_oid=$1
    local marker="pg_textsearch compaction pause at after-restamp"
    local logfile="${PRIMARY_DIR}/log/postgres.log"

    for _ in $(seq 1 300); do
        if grep -Fq "${marker} for index ${index_oid}" \
            "${logfile}" 2>/dev/null; then
            log "Compaction reached the pre-publication pause"
            return 0
        fi
        if ! kill -0 "${COMPACTOR_PID}" 2>/dev/null; then
            error "Compactor exited before the pre-publication pause: \
$(cat "${PRIMARY_DIR}/compactor.out" 2>/dev/null || echo no output)"
        fi
        sleep 0.1
    done

    error "Timed out 30s waiting for pre-publication pause"
}

wait_for_snapshot_pause() {
    local index_oid=$1
    local output=$2
    local marker="pg_textsearch segment graph snapshot pause for index \
${index_oid}"
    local logfile="${STANDBY_DIR}/log/postgres.log"

    for _ in $(seq 1 300); do
        if grep -Fq "${marker}" "${logfile}" 2>/dev/null; then
            log "Standby query copied its complete segment-root snapshot"
            return 0
        fi
        if ! kill -0 "${SNAPSHOT_READER_PID}" 2>/dev/null; then
            wait "${SNAPSHOT_READER_PID}" 2>/dev/null || true
            SNAPSHOT_READER_PID=
            error "Standby snapshot reader exited before its pause: \
$(cat "${output}" 2>/dev/null || echo no output)"
        fi
        sleep 0.1
    done

    error "Timed out 30s waiting for standby segment-root snapshot pause"
}

wait_for_compactor() {
    for _ in $(seq 1 400); do
        if ! kill -0 "${COMPACTOR_PID}" 2>/dev/null; then
            if wait "${COMPACTOR_PID}"; then
                COMPACTOR_PID=
                return 0
            fi
            error "Compactor failed: \
$(cat "${PRIMARY_DIR}/compactor.out" 2>/dev/null || echo no output)"
        fi
        sleep 0.1
    done

    error "Timed out 40s waiting for compactor PID ${COMPACTOR_PID}"
}

wait_for_snapshot_reader() {
    local output=$1

    for _ in $(seq 1 700); do
        if ! kill -0 "${SNAPSHOT_READER_PID}" 2>/dev/null; then
            if wait "${SNAPSHOT_READER_PID}"; then
                SNAPSHOT_READER_PID=
                return 0
            fi
            error "Standby snapshot reader failed: \
$(cat "${output}" 2>/dev/null || echo no output)"
        fi
        sleep 0.1
    done

    error "Timed out 70s waiting for standby snapshot reader PID \
${SNAPSHOT_READER_PID}"
}

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

reader_expect_recovery_conflict() {
    local output
    local reader_status
    local backend_count=1

    for _ in $(seq 1 100); do
        backend_count=$(standby_sql_quiet "
            SELECT count(*) FROM pg_stat_activity
             WHERE pid = ${READER_BACKEND_PID};")
        [ "${backend_count}" = "0" ] && break
        sleep 0.1
    done
    [ "${backend_count}" = "0" ] ||
        error "Old-graph reader was not canceled by recovery conflict WAL"

    printf '%s\n' "SELECT 1;" >&9 || true
    exec 9>&-
    output=$(timeout 10 cat <&8)
    exec 8<&-
    set +e
    wait "${READER_PID}"
    reader_status=$?
    set -e

    READER_OPEN=false
    READER_PID=
    rm -rf "${STANDBY_DIR}/ranked_reader"

    if [ "${reader_status}" -eq 0 ]; then
        error "Old-graph reader completed without a recovery conflict"
    fi
    if ! grep -Eq 'conflict with recovery|recovery conflict' <<<"${output}" &&
       ! grep -Eq 'conflict with recovery|recovery conflict' \
           "${STANDBY_DIR}/log/postgres.log"; then
        error "Old-graph reader failed without a recovery-conflict message: \
${output:-no output}"
    fi
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

test_disconnected_standby_conflict() {
    local graph
    local parked

    log "Case: reclaim WAL conflicts with disconnected standby readers..."
    cat >> "${STANDBY_DIR}/postgresql.conf" <<EOF
max_standby_streaming_delay = 0
EOF
    pg_ctl restart -D "${STANDBY_DIR}" \
        -l "${STANDBY_DIR}/postgres.log" -w -t 30 >/dev/null

    primary_sql "
        CREATE TABLE conflict_rec (
            id integer PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        INSERT INTO conflict_rec
        SELECT g, 'delta epsilon disconnected standby document ' || g
          FROM generate_series(1, 4000) g;
        CREATE INDEX conflict_idx ON conflict_rec USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');
        INSERT INTO conflict_rec
        SELECT g, 'delta epsilon disconnected standby document ' || g
          FROM generate_series(4001, 8000) g;" >/dev/null
    [ "$(primary_sql_quiet \
        "SELECT bm25_spill_index('conflict_idx') > 0;")" = "t" ] ||
        error "Disconnected-standby case did not create a second L0 segment"
    primary_sql "CREATE TABLE conflict_flush_marker (id integer);" >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up before disconnected cursor setup"
    graph=$(standby_sql_quiet \
        "SELECT bm25_level_counts('conflict_idx'::regclass)::text;")
    [ "${graph}" = "{2,0,0,0,0,0,0,0}" ] ||
        error "Disconnected-standby old graph is ${graph}"

    reader_open
    READER_BACKEND_PID=$(reader_query "SELECT pg_backend_pid();")
    reader_query "BEGIN ISOLATION LEVEL REPEATABLE READ;" >/dev/null
    reader_query "SET enable_seqscan = off;" >/dev/null
    reader_query "
        DECLARE held_ranked NO SCROLL CURSOR FOR
        SELECT id
          FROM conflict_rec
         ORDER BY body <@> to_bm25query('delta', 'conflict_idx')
         LIMIT 8000;" >/dev/null
    [ -n "$(reader_query "FETCH FORWARD 1 FROM held_ranked;")" ] ||
        error "Disconnected-standby cursor returned no first document"

    cat >> "${PRIMARY_DIR}/postgresql.conf" <<EOF
max_wal_senders = 0
EOF
    pg_ctl restart -D "${PRIMARY_DIR}" \
        -l "${PRIMARY_DIR}/postgres.log" -w -t 30 >/dev/null
    [ "$(primary_sql_quiet \
        "SELECT count(*) FROM pg_stat_replication;")" = "0" ] ||
        error "Standby remained connected after disabling WAL senders"

    primary_sql "SELECT bm25_force_merge('conflict_idx');" >/dev/null
    for _ in $(seq 1 32); do
        primary_sql_quiet "SELECT txid_current();" >/dev/null
    done
    primary_sql "VACUUM conflict_rec;" >/dev/null
    parked=$(primary_sql_quiet \
        "SELECT bm25_pending_free_pages('conflict_idx');")
    [ "${parked}" = "0" ] ||
        error "Disconnected-standby reclaim left ${parked} pages parked"

    cat >> "${PRIMARY_DIR}/postgresql.conf" <<EOF
max_wal_senders = 8
EOF
    pg_ctl restart -D "${PRIMARY_DIR}" \
        -l "${PRIMARY_DIR}/postgres.log" -w -t 30 >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not replay disconnected-standby reclaim"
    reader_expect_recovery_conflict
    log "PASS: stock WAL replay canceled the disconnected old-graph reader"
}

test_atomic_segment_graph_snapshot() {
    local actual_count actual_rows expected expected_rows graph index_oid
    local output="${STANDBY_DIR}/segment_snapshot_reader.out"
    local result spilled

    log "Case: standby scoring uses one atomic segment-root snapshot..."
    primary_sql "
        CREATE TABLE snapshot_rec (
            id integer PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        INSERT INTO snapshot_rec
        SELECT g, 'atomic snapshot alpha document ' || g
          FROM generate_series(1, 1000) g;
        CREATE INDEX snapshot_idx ON snapshot_rec USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');" >/dev/null

    for batch in $(seq 1 7); do
        primary_sql "
            INSERT INTO snapshot_rec
            SELECT g, 'atomic snapshot alpha document ' || g
              FROM generate_series(
                  ${batch} * 1000 + 1,
                  (${batch} + 1) * 1000) g;" >/dev/null
        spilled=$(primary_sql_quiet \
            "SELECT bm25_spill_index('snapshot_idx') > 0;")
        [ "${spilled}" = "t" ] ||
            error "Snapshot case batch ${batch} did not spill"
    done
    graph=$(primary_sql_quiet \
        "SELECT bm25_level_counts('snapshot_idx'::regclass)::text;")
    [ "${graph}" = "{8,0,0,0,0,0,0,0}" ] ||
        error "Expected eight-L0 snapshot graph, got ${graph}"
    primary_sql "CREATE TABLE snapshot_flush_before (id integer);" >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up before snapshot compaction"

    index_oid=$(primary_sql_quiet \
        "SELECT 'snapshot_idx'::regclass::oid;")
    primary_sql "
        SET pg_textsearch.debug_compaction_pause_after_restamp_ms = 30000;
        SELECT bm25_force_merge('snapshot_idx');" \
        >"${PRIMARY_DIR}/snapshot_compactor.out" 2>&1 &
    COMPACTOR_PID=$!
    wait_for_compaction_pause "${index_oid}"

    primary_sql "
        INSERT INTO snapshot_rec
        SELECT g, 'atomic snapshot alpha document ' || g
          FROM generate_series(8001, 9000) g;" >/dev/null
    spilled=$(primary_sql_quiet \
        "SELECT bm25_spill_index('snapshot_idx') > 0;")
    [ "${spilled}" = "t" ] ||
        error "Snapshot case did not prepend the ninth L0 segment"
    graph=$(primary_sql_quiet \
        "SELECT bm25_level_counts('snapshot_idx'::regclass)::text;")
    [ "${graph}" = "{9,0,0,0,0,0,0,0}" ] ||
        error "Expected ninth prepended L0 segment, got ${graph}"
    primary_sql "CREATE TABLE snapshot_flush_after (id integer);" >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up to the nine-L0 graph"

    rm -f "${output}"
    psql -X -tAq -v ON_ERROR_STOP=1 -p "${STANDBY_PORT}" \
        -d "${TEST_DB}" >"${output}" 2>&1 <<'SQL' &
SET enable_seqscan = off;
SET pg_textsearch.debug_segment_graph_snapshot_pause_ms = 45000;
SELECT id
  FROM snapshot_rec
 ORDER BY body <@> to_bm25query('alpha', 'snapshot_idx')
 LIMIT 9000;
SQL
    SNAPSHOT_READER_PID=$!
    wait_for_snapshot_pause "${index_oid}" "${output}"
    kill -0 "${COMPACTOR_PID}" 2>/dev/null ||
        error "Compactor published before the standby snapshot pause"

    wait_for_compactor
    primary_sql "CREATE TABLE snapshot_flush_publish (id integer);" >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not replay publication during snapshot scoring"
    kill -0 "${SNAPSHOT_READER_PID}" 2>/dev/null ||
        error "Standby snapshot reader resumed before publication replay"
    wait_for_snapshot_reader "${output}"

    result=$(cat "${output}")
    if grep -Ev '^[0-9]+$' <<<"${result}" >/dev/null; then
        error "Standby mixed graph returned a non-numeric document ID"
    fi
    expected=$(primary_sql_quiet \
        "SELECT id FROM snapshot_rec ORDER BY id;")
    expected_rows="$(printf '%s\n' "${expected}" | wc -l | tr -d ' ')"
    actual_count="$(printf '%s\n' "${result}" | sort -n | uniq | wc -l)"
    actual_rows="$(printf '%s\n' "${result}" | wc -l)"
    [ "${actual_rows}" = "${expected_rows}" ] ||
        error "standby mixed graph returned ${actual_rows}/${expected_rows} rows"
    [ "${actual_count}" = "${expected_rows}" ] ||
        error "standby mixed graph returned duplicate document IDs"
    [ "$(printf '%s\n' "${result}" | sort -n)" = "${expected}" ] ||
        error "standby mixed graph IDs differ from the heap-derived set"

    log "PASS: standby scoring returned all ${expected_rows} IDs exactly once"
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
        CREATE TABLE reclaim_xid_advance (id integer PRIMARY KEY);
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

    primary_sql "
        SET pg_textsearch.debug_compaction_pause_after_restamp_ms = 15000;
        SELECT bm25_force_merge('rec_idx');" \
        >"${PRIMARY_DIR}/compactor.out" 2>&1 &
    COMPACTOR_PID=$!
    wait_for_compaction_pause \
        "$(primary_sql_quiet "SELECT 'rec_idx'::regclass::oid;")"

    # Advance the primary and standby XID horizons while the old graph is
    # still published.  The cursor opened below must therefore be protected
    # by the publication-time reclaim stamp, not the earlier build start.
    for xid_id in $(seq 1 32); do
        primary_sql_quiet "
            INSERT INTO reclaim_xid_advance VALUES (${xid_id});" >/dev/null
    done
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up before late ranked cursor setup"

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
    graph=$(standby_sql_quiet \
        "SELECT bm25_level_counts('rec_idx'::regclass)::text;")
    [ "${graph}" = "{2,0,0,0,0,0,0,0}" ] ||
        error "Late ranked cursor did not start on the old graph: ${graph}"
    wait_for_feedback_xmin
    log "Late ranked cursor PID ${READER_BACKEND_PID} is open on the old graph"

    wait_for_compactor
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

    # PostgreSQL 18 can hold publication redo behind the cursor's buffer
    # pin. Fetch the old graph to completion before requiring replay to
    # advance; waiting for catch-up first would deadlock until
    # max_standby_streaming_delay cancels the cursor.
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
after primary publication"

    reader_query "CLOSE held_ranked; COMMIT;" >/dev/null
    reader_close
    wait_for_feedback_release
    wait_for_standby_catchup 30 ||
        error "Standby did not replay publication after the old cursor closed"

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
    test_disconnected_standby_conflict
    test_atomic_segment_graph_snapshot
    log "All standby reclaim overlap checks passed"
}

main "$@"
