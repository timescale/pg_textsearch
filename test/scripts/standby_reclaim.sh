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
# shellcheck source=standby_conflict_output.sh
source "${SCRIPT_DIR}/standby_conflict_output.sh"

READER_PID=
READER_BACKEND_PID=
READER_OPEN=false
HELD_FEEDBACK_XMIN=
COMPACTOR_PID=
COMPACTOR_BACKEND_PID=
SPILLER_PID=
SPILLER_BACKEND_PID=
SPILL_GATE_PID=
SPILL_GATE_OPEN=false
SNAPSHOT_READER_PID=
SNAPSHOT_READER_BACKEND_PID=

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
    if [ -n "${COMPACTOR_BACKEND_PID}" ]; then
        kill -CONT "${COMPACTOR_BACKEND_PID}" 2>/dev/null || true
    fi
    if [ -n "${SPILLER_BACKEND_PID}" ]; then
        kill -CONT "${SPILLER_BACKEND_PID}" 2>/dev/null || true
    fi
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
    if [ -n "${SPILLER_PID}" ] &&
       kill -0 "${SPILLER_PID}" 2>/dev/null; then
        kill -TERM "${SPILLER_PID}" 2>/dev/null || true
        wait_for_child_exit "${SPILLER_PID}" 50 || true
    fi
    if [ "${SPILL_GATE_OPEN}" = "true" ]; then
        exec 7>&- || true
        SPILL_GATE_OPEN=false
    fi
    if [ -n "${SPILL_GATE_PID}" ] &&
       kill -0 "${SPILL_GATE_PID}" 2>/dev/null; then
        kill -TERM "${SPILL_GATE_PID}" 2>/dev/null || true
        wait_for_child_exit "${SPILL_GATE_PID}" 50 || true
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
    local output=$2
    local marker="pg_textsearch compaction pause at after-restamp"
    local logfile="${PRIMARY_DIR}/log/postgres.log"
    local marker_line

    for _ in $(seq 1 300); do
        marker_line=$(grep -F "${marker} for index ${index_oid}" \
            "${logfile}" 2>/dev/null | tail -1 || true)
        if [ -n "${marker_line}" ]; then
            COMPACTOR_BACKEND_PID=$(sed -n \
                's/.* backend \([0-9][0-9]*\).*/\1/p' <<<"${marker_line}")
            [ -n "${COMPACTOR_BACKEND_PID}" ] ||
                error "Could not parse compactor backend PID: ${marker_line}"
            log "Compaction reached the pre-publication pause"
            return 0
        fi
        if ! kill -0 "${COMPACTOR_PID}" 2>/dev/null; then
            error "Compactor exited before the pre-publication pause: \
$(cat "${output}" 2>/dev/null || echo no output)"
        fi
        sleep 0.1
    done

    error "Timed out 30s waiting for pre-publication pause"
}

wait_for_spill_pause() {
    local index_oid=$1
    local output=$2
    local marker="pg_textsearch spill before-finalize gate"
    local logfile="${PRIMARY_DIR}/log/postgres.log"
    local marker_line

    for _ in $(seq 1 300); do
        marker_line=$(grep -F "${marker} for index ${index_oid}" \
            "${logfile}" 2>/dev/null | tail -1 || true)
        if [ -n "${marker_line}" ]; then
            SPILLER_BACKEND_PID=$(sed -n \
                's/.* backend \([0-9][0-9]*\).*/\1/p' <<<"${marker_line}")
            [ -n "${SPILLER_BACKEND_PID}" ] ||
                error "Could not parse spiller backend PID: ${marker_line}"
            log "Spill reached the pre-finalize gate"
            return 0
        fi
        if ! kill -0 "${SPILLER_PID}" 2>/dev/null; then
            error "Spiller exited before the pre-finalize pause: \
$(cat "${output}" 2>/dev/null || echo no output)"
        fi
        sleep 0.1
    done

    error "Timed out 30s waiting for pre-finalize spill gate"
}

wait_for_snapshot_pause() {
    local phase=$1
    local index_oid=$2
    local output=$3
    local marker="pg_textsearch segment graph snapshot pause at ${phase} \
for index ${index_oid}"
    local logfile="${STANDBY_DIR}/log/postgres.log"

    for _ in $(seq 1 300); do
        if grep -Fq "${marker}" "${logfile}" 2>/dev/null; then
            log "Standby query reached segment snapshot phase ${phase}"
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
    local output=$1

    for _ in $(seq 1 400); do
        if ! kill -0 "${COMPACTOR_PID}" 2>/dev/null; then
            if wait "${COMPACTOR_PID}"; then
                COMPACTOR_PID=
                return 0
            fi
            error "Compactor failed: \
$(cat "${output}" 2>/dev/null || echo no output)"
        fi
        sleep 0.1
    done

    error "Timed out 40s waiting for compactor PID ${COMPACTOR_PID}"
}

wait_for_spiller() {
    local output=$1

    for _ in $(seq 1 400); do
        if ! kill -0 "${SPILLER_PID}" 2>/dev/null; then
            if wait "${SPILLER_PID}"; then
                SPILLER_PID=
                return 0
            fi
            error "Spiller failed: \
$(cat "${output}" 2>/dev/null || echo no output)"
        fi
        sleep 0.1
    done

    error "Timed out 40s waiting for spiller PID ${SPILLER_PID}"
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

snapshot_reader_backend_pid() {
    local output=$1
    local deadline=$((SECONDS + 10))
    local pid

    while ((SECONDS < deadline)); do
        pid=$(sed -n '1p' "${output}" 2>/dev/null || true)
        if [[ "${pid}" =~ ^[0-9]+$ ]]; then
            echo "${pid}"
            return
        fi
        sleep 0.05
    done
    error "Standby snapshot reader did not report its backend PID"
}

wait_for_snapshot_reader_conflict() {
    local output=$1
    local deadline=$((SECONDS + 45))
    local status

    while kill -0 "${SNAPSHOT_READER_PID}" 2>/dev/null; do
        ((SECONDS < deadline)) ||
            error "Old-memtable reader was not canceled by conflict WAL"
        sleep 0.1
    done

    set +e
    wait "${SNAPSHOT_READER_PID}"
    status=$?
    set -e
    SNAPSHOT_READER_PID=
    [ "${status}" -ne 0 ] ||
        error "Old-memtable reader completed without a recovery conflict"
    snapshot_reader_conflict_output_is_valid "${output}" ||
        error "Old-memtable reader lacks scoped conflict proof or reported corruption"
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
    local pre_publish_lsn replay_blocked result spilled target_lsn

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
        SET pg_textsearch.debug_compaction_pause_after_restamp_ms = 5000;
        SELECT bm25_force_merge('snapshot_idx');" \
        >"${PRIMARY_DIR}/snapshot_compactor.out" 2>&1 &
    COMPACTOR_PID=$!
    wait_for_compaction_pause \
        "${index_oid}" "${PRIMARY_DIR}/snapshot_compactor.out"
    kill -STOP "${COMPACTOR_BACKEND_PID}"
    sleep 6

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
SET pg_textsearch.debug_segment_graph_snapshot_pause_before_unlock_ms = 15000;
SET pg_textsearch.debug_segment_graph_snapshot_pause_ms = 15000;
SELECT id
  FROM snapshot_rec
 ORDER BY body <@> to_bm25query('alpha', 'snapshot_idx')
 LIMIT 9000;
SQL
    SNAPSHOT_READER_PID=$!
    wait_for_snapshot_pause before-unlock "${index_oid}" "${output}"
    pre_publish_lsn=$(primary_sql_quiet \
        "SELECT pg_current_wal_flush_lsn();")
    kill -CONT "${COMPACTOR_BACKEND_PID}"
    wait_for_compactor "${PRIMARY_DIR}/snapshot_compactor.out"
    COMPACTOR_BACKEND_PID=
    primary_sql "CREATE TABLE snapshot_flush_publish (id integer);" >/dev/null
    target_lsn=$(primary_sql_quiet "SELECT pg_current_wal_flush_lsn();")

    replay_blocked=f
    for _ in $(seq 1 50); do
        replay_blocked=$(primary_sql_quiet "
            SELECT coalesce(bool_or(
                       replay_lsn >= '${pre_publish_lsn}'::pg_lsn
                   AND replay_lsn < '${target_lsn}'::pg_lsn), false)
              FROM pg_stat_replication;")
        [ "${replay_blocked}" = "t" ] && break
        sleep 0.1
    done
    [ "${replay_blocked}" = "t" ] ||
        error "Standby replay was not blocked by the metapage share lock"
    kill -0 "${SNAPSHOT_READER_PID}" 2>/dev/null ||
        error "Standby snapshot reader left its pre-unlock pause early"
    log "Standby replay remained blocked during the pre-unlock pause"

    wait_for_snapshot_pause after-unlock "${index_oid}" "${output}"
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

test_atomic_ranked_memtable_generation() {
    local actual_count actual_rows expected index_oid output result spilled

    log "Case: standby ranked scoring uses one segment+memtable generation..."
    primary_sql "
        CREATE TABLE ranked_generation_rec (
            id integer PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        INSERT INTO ranked_generation_rec
        SELECT g, 'ranked generation alpha base ' || g
          FROM generate_series(1, 1000) g;
        CREATE INDEX ranked_generation_idx
            ON ranked_generation_rec USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');
        INSERT INTO ranked_generation_rec
        SELECT g, 'ranked generation alpha memtable ' || g
          FROM generate_series(1001, 1500) g;
        CREATE TABLE ranked_generation_flush_before (id integer);" >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up before ranked generation race"

    index_oid=$(primary_sql_quiet \
        "SELECT 'ranked_generation_idx'::regclass::oid;")
    output="${STANDBY_DIR}/ranked_generation_reader.out"
    rm -f "${output}"
    psql -X -tAq -v ON_ERROR_STOP=1 -p "${STANDBY_PORT}" \
        -d "${TEST_DB}" >"${output}" 2>&1 <<'SQL' &
SET enable_seqscan = off;
SET pg_textsearch.debug_segment_graph_snapshot_pause_ms = 15000;
SELECT id
  FROM ranked_generation_rec
 ORDER BY body <@> to_bm25query('alpha', 'ranked_generation_idx')
 LIMIT 1500;
SQL
    SNAPSHOT_READER_PID=$!
    wait_for_snapshot_pause after-unlock "${index_oid}" "${output}"

    spilled=$(primary_sql_quiet \
        "SELECT bm25_spill_index('ranked_generation_idx') > 0;")
    [ "${spilled}" = "t" ] ||
        error "Ranked generation case did not spill the captured memtable"
    primary_sql "CREATE TABLE ranked_generation_flush_after (id integer);" \
        >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not replay ranked generation spill"
    kill -0 "${SNAPSHOT_READER_PID}" 2>/dev/null ||
        error "Ranked generation reader resumed before spill replay"
    wait_for_snapshot_reader "${output}"

    result=$(cat "${output}")
    if grep -Ev '^[0-9]+$' <<<"${result}" >/dev/null; then
        error "Ranked generation race returned a non-numeric document ID"
    fi
    expected=$(seq 1 1500)
    actual_rows="$(printf '%s\n' "${result}" | wc -l | tr -d ' ')"
    actual_count="$(printf '%s\n' "${result}" | sort -n | uniq | wc -l)"
    [ "${actual_rows}" = "1500" ] ||
        error "Ranked generation race returned ${actual_rows}/1500 rows"
    [ "${actual_count}" = "1500" ] ||
        error "Ranked generation race returned duplicate document IDs"
    [ "$(printf '%s\n' "${result}" | sort -n)" = "${expected}" ] ||
        error "Ranked generation race omitted or invented document IDs"
    log "PASS: ranked standby race returned IDs 1..1500 exactly once"
}

test_atomic_standalone_memtable_generation() {
    local baseline index_oid output result spilled

    log "Case: standby standalone scoring uses one segment+memtable generation..."
    primary_sql "
        CREATE TABLE standalone_generation_rec (
            id integer PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        INSERT INTO standalone_generation_rec
        SELECT g, 'standalone generation base ' || g
          FROM generate_series(1, 1000) g;
        CREATE INDEX standalone_generation_idx
            ON standalone_generation_rec USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');
        INSERT INTO standalone_generation_rec
        SELECT g, 'raceword standalone memtable ' || g
          FROM generate_series(1001, 1500) g;
        CREATE TABLE standalone_generation_flush_before (id integer);" \
        >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up before standalone generation race"

    index_oid=$(primary_sql_quiet \
        "SELECT 'standalone_generation_idx'::regclass::oid;")
    output="${STANDBY_DIR}/standalone_generation_reader.out"
    rm -f "${output}"
    psql -X -tAq -v ON_ERROR_STOP=1 -p "${STANDBY_PORT}" \
        -d "${TEST_DB}" >"${output}" 2>&1 <<'SQL' &
SET pg_textsearch.debug_segment_graph_snapshot_pause_before_lock_ms = 15000;
SET pg_textsearch.debug_segment_graph_snapshot_pause_ms = 15000;
SELECT round((
           body <@> to_bm25query(
               'raceword', 'standalone_generation_idx')
       )::numeric, 8)
  FROM standalone_generation_rec
 WHERE id = 1001;
SQL
    SNAPSHOT_READER_PID=$!
    wait_for_snapshot_pause before-lock "${index_oid}" "${output}"

    spilled=$(primary_sql_quiet \
        "SELECT bm25_spill_index('standalone_generation_idx') > 0;")
    [ "${spilled}" = "t" ] ||
        error "Standalone generation case did not spill the first memtable"
    primary_sql "
        INSERT INTO standalone_generation_rec
        SELECT g, 'laterword replacement memtable ' || g
          FROM generate_series(1501, 1800) g;
        CREATE TABLE standalone_generation_flush_middle (id integer);" \
        >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not replay the first standalone spill"

    baseline=$(standby_sql_quiet "
        SELECT round((
                   body <@> to_bm25query(
                       'raceword', 'standalone_generation_idx')
               )::numeric, 8)
          FROM standalone_generation_rec
         WHERE id = 1001;")
    [ -n "${baseline}" ] && [ "${baseline}" != "0.00000000" ] ||
        error "Standalone generation baseline score is ${baseline:-empty}"

    wait_for_snapshot_pause after-unlock "${index_oid}" "${output}"
    spilled=$(primary_sql_quiet \
        "SELECT bm25_spill_index('standalone_generation_idx') > 0;")
    [ "${spilled}" = "t" ] ||
        error "Standalone generation case did not spill the captured endpoint"
    primary_sql "CREATE TABLE standalone_generation_flush_after (id integer);" \
        >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not replay the second standalone spill"
    kill -0 "${SNAPSHOT_READER_PID}" 2>/dev/null ||
        error "Standalone generation reader resumed before spill replay"
    wait_for_snapshot_reader "${output}"

    result=$(cat "${output}")
    [ "${result}" = "${baseline}" ] ||
        error "Standalone generation score ${result}, expected ${baseline}"
    log "PASS: standalone standby race preserved score ${baseline}"
}

test_memtable_retire_horizon_covers_prepublish_reader() {
    local dead_fxid gate_dir gate_key gate_locked index_oid output
    local reader_xmin spill_output

    log "Case: memtable retire horizon covers pre-publication readers..."
    primary_sql "
        CREATE TABLE retire_horizon_rec (
            id integer PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        INSERT INTO retire_horizon_rec
        SELECT g, 'retire horizon committed segment ' || g
          FROM generate_series(1, 1000) g;
        CREATE INDEX retire_horizon_idx
            ON retire_horizon_rec USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');
        INSERT INTO retire_horizon_rec
        SELECT g, 'retire horizon old memtable ' || g
          FROM generate_series(1001, 1500) g;
        CREATE TABLE retire_horizon_flush_before (id integer);" >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up before retire-horizon race"

    index_oid=$(primary_sql_quiet \
        "SELECT 'retire_horizon_idx'::regclass::oid;")
    gate_key=472495
    gate_dir="${PRIMARY_DIR}/retire_horizon_gate"
    rm -rf "${gate_dir}"
    mkdir -p "${gate_dir}"
    mkfifo "${gate_dir}/in"
    psql -X -tAq -v ON_ERROR_STOP=1 -p "${PRIMARY_PORT}" \
        -d "${TEST_DB}" <"${gate_dir}/in" >"${gate_dir}/out" \
        2>"${gate_dir}/err" &
    SPILL_GATE_PID=$!
    exec 7>"${gate_dir}/in"
    SPILL_GATE_OPEN=true
    printf 'SELECT pg_advisory_lock(%s, 0);\n' "${gate_key}" >&7
    gate_locked=false
    for _ in $(seq 1 100); do
        if [ "$(primary_sql_quiet "
            SELECT count(*)
              FROM pg_locks
             WHERE locktype = 'advisory'
                AND classid = ${gate_key}
                AND objid = 0
                AND objsubid = 2
               AND mode = 'ExclusiveLock'
               AND granted;")" = "1" ]; then
            gate_locked=true
            break
        fi
        kill -0 "${SPILL_GATE_PID}" 2>/dev/null ||
            error "Spill gate session exited before acquiring its lock"
        sleep 0.1
    done
    [ "${gate_locked}" = "true" ] ||
        error "Timed out waiting for spill advisory gate"

    spill_output="${PRIMARY_DIR}/retire_horizon_spill.out"
    rm -f "${spill_output}"
    psql -X -tAq -v ON_ERROR_STOP=1 -p "${PRIMARY_PORT}" \
        -d "${TEST_DB}" >"${spill_output}" 2>&1 <<'SQL' &
SET pg_textsearch.debug_spill_before_finalize_gate = 472495;
SELECT bm25_spill_index('retire_horizon_idx') > 0;
SQL
    SPILLER_PID=$!
    wait_for_spill_pause "${index_oid}" "${spill_output}"

    primary_sql "
        SELECT txid_current();
        CREATE TABLE retire_horizon_flush_between (id integer);" >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not replay the intervening transaction"

    output="${STANDBY_DIR}/retire_horizon_reader.out"
    rm -f "${output}"
    psql -X -tAq -v ON_ERROR_STOP=1 -p "${STANDBY_PORT}" \
        -d "${TEST_DB}" >"${output}" 2>&1 <<'SQL' &
SELECT pg_backend_pid();
SET enable_seqscan = off;
SET pg_textsearch.debug_segment_graph_snapshot_pause_ms = 30000;
SELECT id
  FROM retire_horizon_rec
 ORDER BY body <@> to_bm25query('retire', 'retire_horizon_idx')
 LIMIT 1500;
SQL
    SNAPSHOT_READER_PID=$!
    SNAPSHOT_READER_BACKEND_PID=$(snapshot_reader_backend_pid "${output}")
    wait_for_snapshot_pause after-unlock "${index_oid}" "${output}"
    reader_xmin=$(standby_sql_quiet "
        SELECT backend_xmin
          FROM pg_stat_activity
         WHERE pid = ${SNAPSHOT_READER_BACKEND_PID};")
    [ -n "${reader_xmin}" ] ||
        error "Pre-publication reader has no active snapshot xmin"

    printf 'SELECT pg_advisory_unlock(%s, 0);\n' "${gate_key}" >&7
    exec 7>&-
    SPILL_GATE_OPEN=false
    wait "${SPILL_GATE_PID}" ||
        error "Spill gate session failed: $(cat "${gate_dir}/err")"
    SPILL_GATE_PID=
    wait_for_spiller "${spill_output}"
    SPILLER_BACKEND_PID=
    dead_fxid=$(primary_sql_quiet "
        SELECT min(dead_fxid)
          FROM bm25_memtable_dead_pages('retire_horizon_idx');")
    [ -n "${dead_fxid}" ] ||
        error "Retire-horizon spill produced no DEAD pages"

    [ "${dead_fxid}" -ge "${reader_xmin}" ] ||
        error "DEAD horizon ${dead_fxid} does not cover newer old-chain \
reader xmin ${reader_xmin}"

    kill -TERM "${SNAPSHOT_READER_PID}" 2>/dev/null || true
    wait_for_child_exit "${SNAPSHOT_READER_PID}" 50 || true
    SNAPSHOT_READER_PID=
    log "PASS: DEAD horizon ${dead_fxid} covers reader xmin ${reader_xmin}"
}

test_disconnected_memtable_reuse_conflict() {
    local conflict_end_lsn conflict_records conflict_start_lsn dead_after
    local dead_before dead_fxid expected_ids final_ids graph index_oid output
    local pending reader_xmin reused spilled

    log "Case: reclaimed memtable reuse cancels disconnected old readers..."
    cat >> "${STANDBY_DIR}/postgresql.conf" <<EOF
hot_standby_feedback = off
EOF
    pg_ctl restart -D "${STANDBY_DIR}" \
        -l "${STANDBY_DIR}/postgres.log" -w -t 30 >/dev/null

    primary_sql "
        CREATE TABLE memreuse_rec (
            id integer PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        INSERT INTO memreuse_rec
        SELECT g, 'memreuse committed segment ' || g
          FROM generate_series(1, 1000) g;
        CREATE INDEX memreuse_idx ON memreuse_rec USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');
        INSERT INTO memreuse_rec
        SELECT g, 'memreuse retired memtable ' || g || ' ' ||
                  repeat(md5(g::text), 8)
          FROM generate_series(1001, 1500) g;
        CREATE TABLE memreuse_flush_before (id integer);" >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up before memtable reuse conflict"

    index_oid=$(primary_sql_quiet "SELECT 'memreuse_idx'::regclass::oid;")
    output="${STANDBY_DIR}/memreuse_reader.out"
    rm -f "${output}"
    psql -X -tAq -v ON_ERROR_STOP=1 -p "${STANDBY_PORT}" \
        -d "${TEST_DB}" >"${output}" 2>&1 <<'SQL' &
SELECT pg_backend_pid();
SET enable_seqscan = off;
SET pg_textsearch.debug_segment_graph_snapshot_pause_ms = 30000;
SELECT id
  FROM memreuse_rec
 ORDER BY body <@> to_bm25query('memreuse', 'memreuse_idx')
 LIMIT 1500;
SQL
    SNAPSHOT_READER_PID=$!
    SNAPSHOT_READER_BACKEND_PID=$(snapshot_reader_backend_pid "${output}")
    wait_for_snapshot_pause after-unlock "${index_oid}" "${output}"
    reader_xmin=$(standby_sql_quiet "
        SELECT backend_xmin
          FROM pg_stat_activity
         WHERE pid = ${SNAPSHOT_READER_BACKEND_PID};")
    [ -n "${reader_xmin}" ] ||
        error "Old-memtable reader has no active snapshot xmin"

    cat >> "${PRIMARY_DIR}/postgresql.conf" <<EOF
max_wal_senders = 0
EOF
    pg_ctl restart -D "${PRIMARY_DIR}" \
        -l "${PRIMARY_DIR}/postgres.log" -w -t 30 >/dev/null
    [ "$(primary_sql_quiet "SELECT count(*) FROM pg_stat_replication;")" = "0" ] ||
        error "Standby remained connected before memtable reclaim"

    spilled=$(primary_sql_quiet \
        "SELECT bm25_spill_index('memreuse_idx') > 0;")
    [ "${spilled}" = "t" ] ||
        error "Memtable reuse case did not spill the captured chain"
    dead_before=$(primary_sql_quiet \
        "SELECT count(*) FROM bm25_memtable_dead_pages('memreuse_idx');")
    [ "${dead_before}" -gt 0 ] ||
        error "Memtable reuse case produced no DEAD pages"
    primary_sql "
        DROP TABLE IF EXISTS memreuse_dead_blocks;
        CREATE TABLE memreuse_dead_blocks AS
        SELECT blkno FROM bm25_memtable_dead_pages('memreuse_idx');
        ALTER TABLE memreuse_dead_blocks ADD PRIMARY KEY (blkno);" >/dev/null
    dead_fxid=$(primary_sql_quiet "
        SELECT min(dead_fxid)
          FROM bm25_memtable_dead_pages('memreuse_idx');")
    log "Old reader xmin=${reader_xmin}; DEAD horizon=${dead_fxid}"
    pending=$(primary_sql_quiet \
        "SELECT bm25_pending_free_pages('memreuse_idx');")
    [ "${pending}" = "0" ] ||
        error "Memtable reuse case unexpectedly created segment tombstones"

    for _ in $(seq 1 64); do
        primary_sql_quiet "SELECT txid_current();" >/dev/null
    done
    conflict_start_lsn=$(primary_sql_quiet \
        "SELECT pg_current_wal_insert_lsn();")
    primary_sql "VACUUM memreuse_rec;" >/dev/null
    dead_after=$(primary_sql_quiet \
        "SELECT count(*) FROM bm25_memtable_dead_pages('memreuse_idx');")
    [ "${dead_after}" = "0" ] ||
        error "VACUUM left ${dead_after}/${dead_before} DEAD memtable pages"

    primary_sql "
        SET pg_textsearch.memtable_pages_threshold = 0;
        INSERT INTO memreuse_rec
        SELECT g, 'memreuse replacement chain ' || g || ' ' ||
                  repeat(md5(g::text), 8)
          FROM generate_series(1501, 2000) g;" >/dev/null
    reused=$(primary_sql_quiet "
        SELECT count(*)
          FROM bm25_memtable_chain('memreuse_idx') chain
          JOIN memreuse_dead_blocks dead USING (blkno);")
    [ "${reused}" -gt 0 ] ||
        error "Replacement memtable did not reuse a reclaimed DEAD page"
    conflict_end_lsn=$(primary_sql_quiet \
        "SELECT pg_current_wal_insert_lsn();")
    conflict_records=$(pg_waldump -p "${PRIMARY_DIR}/pg_wal" \
        -s "${conflict_start_lsn}" -e "${conflict_end_lsn}" -r Btree |
        grep -c "REUSE_PAGE" || true)
    [ "${conflict_records}" -ge "${dead_before}" ] ||
        error "WAL contains ${conflict_records}/${dead_before} memtable reuse conflicts"
    log "WAL contains ${conflict_records} memtable reuse conflict records"

    cat >> "${PRIMARY_DIR}/postgresql.conf" <<EOF
max_wal_senders = 8
EOF
    pg_ctl restart -D "${PRIMARY_DIR}" \
        -l "${PRIMARY_DIR}/postgres.log" -w -t 30 >/dev/null
    wait_for_snapshot_reader_conflict "${output}"
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up after memtable reuse conflict"

    graph=$(standby_sql_quiet \
        "SELECT bm25_level_counts('memreuse_idx'::regclass)::text;")
    [ "${graph}" = "{2,0,0,0,0,0,0,0}" ] ||
        error "Memtable reuse case changed segment graph unexpectedly: ${graph}"
    expected_ids=$(seq 1 2000 | paste -sd, -)
    final_ids=$(standby_sql_quiet "
        SET enable_seqscan = off;
        SELECT string_agg(id::text, ',' ORDER BY id)
          FROM (
                SELECT id FROM memreuse_rec
                 ORDER BY body <@> to_bm25query('memreuse', 'memreuse_idx')
                 LIMIT 2000
               ) ranked;" | tail -n 1)
    [ "${final_ids}" = "${expected_ids}" ] ||
        error "Standby IDs are wrong after memtable conflict and reuse"
    log "PASS: conflict WAL canceled old memtable reader before ${reused} reused pages"
}

test_ranked_promotion_pins_recovery_mode() {
    local actual_count actual_rows expected index_oid output result spilled

    log "Case: ranked scoring pins recovery mode before its snapshot..."
    primary_sql "
        CREATE TABLE promotion_generation_rec (
            id integer PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        INSERT INTO promotion_generation_rec
        SELECT g, 'promotion generation alpha segment ' || g
          FROM generate_series(1, 1000) g;
        CREATE INDEX promotion_generation_idx
            ON promotion_generation_rec USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');
        INSERT INTO promotion_generation_rec
        SELECT g, 'promotion generation alpha memtable ' || g
          FROM generate_series(1001, 1500) g;
        CREATE TABLE promotion_generation_flush_before (id integer);" \
        >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not catch up before promotion generation race"

    index_oid=$(primary_sql_quiet \
        "SELECT 'promotion_generation_idx'::regclass::oid;")
    output="${STANDBY_DIR}/promotion_generation_reader.out"
    rm -f "${output}"
    psql -X -tAq -v ON_ERROR_STOP=1 -p "${STANDBY_PORT}" \
        -d "${TEST_DB}" >"${output}" 2>&1 <<'SQL' &
SELECT pg_backend_pid();
SET enable_seqscan = off;
SET pg_textsearch.debug_segment_graph_snapshot_pause_ms = 15000;
SELECT id
  FROM promotion_generation_rec
 ORDER BY body <@> to_bm25query('alpha', 'promotion_generation_idx')
 LIMIT 1500;
SQL
    SNAPSHOT_READER_PID=$!
    wait_for_snapshot_pause after-unlock "${index_oid}" "${output}"

    spilled=$(primary_sql_quiet \
        "SELECT bm25_spill_index('promotion_generation_idx') > 0;")
    [ "${spilled}" = "t" ] ||
        error "Promotion generation case did not spill the captured memtable"
    primary_sql "CREATE TABLE promotion_generation_flush_after (id integer);" \
        >/dev/null
    wait_for_standby_catchup 30 ||
        error "Standby did not replay promotion generation spill"

    pg_ctl promote -D "${STANDBY_DIR}" -w >/dev/null
    [ "$(standby_sql_quiet "SELECT pg_is_in_recovery();")" = "f" ] ||
        error "Standby did not promote during ranked snapshot pause"
    wait_for_snapshot_reader "${output}"

    result=$(tail -n +2 "${output}")
    if grep -Ev '^[0-9]+$' <<<"${result}" >/dev/null; then
        error "Promotion generation race returned a non-numeric ID"
    fi
    expected=$(seq 1 1500)
    actual_rows="$(printf '%s\n' "${result}" | wc -l | tr -d ' ')"
    actual_count="$(printf '%s\n' "${result}" | sort -n | uniq | wc -l)"
    [ "${actual_rows}" = "1500" ] && [ "${actual_count}" = "1500" ] ||
        error "Promotion generation race returned ${actual_rows} rows, ${actual_count} unique"
    [ "$(printf '%s\n' "${result}" | sort -n)" = "${expected}" ] ||
        error "Promotion generation race omitted or duplicated IDs"
    log "PASS: promotion race returned IDs 1..1500 exactly once"
}

main() {
    local spilled graph feedback_setting plan first_id remaining
    local parked_before_vacuum parked_after_vacuum drained
    local expected_ids primary_ids standby_ids
    local held_ids held_ids_sorted held_id_count duplicate_ids

    check_required_tools
    command -v pg_waldump >/dev/null 2>&1 ||
        error "pg_waldump not found"
    setup_primary
    cat >> "${PRIMARY_DIR}/postgresql.conf" <<EOF
wal_keep_size = '128MB'
EOF
    pg_ctl restart -D "${PRIMARY_DIR}" \
        -l "${PRIMARY_DIR}/postgres.log" -w -t 30 >/dev/null
    [ "$(primary_sql_quiet "SHOW wal_keep_size;")" = "128MB" ] ||
        error "Primary did not retain the disconnected standby WAL window"

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
        "$(primary_sql_quiet "SELECT 'rec_idx'::regclass::oid;")" \
        "${PRIMARY_DIR}/compactor.out"

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

    wait_for_compactor "${PRIMARY_DIR}/compactor.out"
    COMPACTOR_BACKEND_PID=
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
    test_atomic_ranked_memtable_generation
    test_atomic_standalone_memtable_generation
    test_memtable_retire_horizon_covers_prepublish_reader
    test_disconnected_memtable_reuse_conflict
    test_ranked_promotion_pins_recovery_mode
    log "All standby reclaim overlap checks passed"
}

main "$@"
