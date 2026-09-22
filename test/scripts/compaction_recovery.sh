#!/bin/bash
#
# Verify compaction publication crash recovery and standby mutator guards.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55464
STANDBY_PORT=55465
TEST_DB=compaction_recovery_test
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_compaction_recovery_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_compaction_recovery_standby"

# Avoid Unix-socket path limits in deeply nested worktrees.
REPL_HOST=127.0.0.1
REPL_SOCKET_DIR=

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

primary_value() {
    primary_sql_quiet "$1"
}

primary_ranked_value() {
    PGOPTIONS="-c enable_seqscan=off" primary_sql_quiet "$1"
}

wait_for_postmaster_exit() {
    local pid=$1
    local label=$2
    local deadline=$((SECONDS + 10))

    while kill -0 "${pid}" 2>/dev/null; do
        ((SECONDS < deadline)) ||
            error "${label} postmaster ${pid} did not exit"
        sleep 0.05
    done
}

restart_primary() {
    rm -f "${PRIMARY_DIR}/postmaster.pid"
    pg_ctl start -D "${PRIMARY_DIR}" \
        -l "${PRIMARY_DIR}/postgres.log" -w
    primary_value "SELECT 1;" >/dev/null
}

log_contains() {
    local marker=$1

    grep -Fq "${marker}" "${PRIMARY_DIR}/postgres.log" \
        "${PRIMARY_DIR}/log/postgres.log" 2>/dev/null
}

wait_for_backend() {
    local app_name=$1
    local deadline=$((SECONDS + 10))
    local pid

    while ((SECONDS < deadline)); do
        pid=$(primary_value "
            SELECT pid
              FROM pg_stat_activity
             WHERE application_name = '${app_name}'
             ORDER BY backend_start DESC
             LIMIT 1;")
        if [[ "${pid}" =~ ^[0-9]+$ ]]; then
            echo "${pid}"
            return
        fi
        sleep 0.05
    done
    error "backend ${app_name} did not appear"
}

wait_for_injection() {
    local point=$1
    local backend=$2
    local deadline=$((SECONDS + 10))

    while ((SECONDS < deadline)); do
        if [ "$(primary_value "
                SELECT EXISTS (
                    SELECT 1
                      FROM pg_stat_activity
                     WHERE pid = ${backend}
                       AND wait_event_type = 'InjectionPoint'
                       AND wait_event = '${point}'
                );")" = "t" ]; then
            log "Backend ${backend} is waiting at ${point}"
            return
        fi
        sleep 0.05
    done
    error "backend ${backend} did not reach injection point ${point}"
}

create_crash_fixture() {
    local prefix=$1
    local spilled
    local graph

    primary_sql "
        CREATE TABLE ${prefix}_docs (
            id integer PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        INSERT INTO ${prefix}_docs
        SELECT g, 'crashmarker recovery document ' || g
          FROM generate_series(1, 300) g;
        CREATE INDEX ${prefix}_idx
            ON ${prefix}_docs USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');
        INSERT INTO ${prefix}_docs
        SELECT g, 'crashmarker recovery document ' || g
          FROM generate_series(301, 600) g;
    " >/dev/null

    spilled=$(primary_value \
        "SELECT bm25_spill_index('${prefix}_idx') > 0;")
    [ "${spilled}" = "t" ] ||
        error "${prefix}: second segment was not spilled"
    graph=$(primary_value \
        "SELECT bm25_level_counts('${prefix}_idx'::regclass)::text;")
    [ "${graph}" = "{2,0,0,0,0,0,0,0}" ] ||
        error "${prefix}: fixture graph is ${graph}"
    primary_sql "CHECKPOINT;" >/dev/null
}

crash_after_detached_wal() {
    local prefix=$1
    local app_name="pgts-${prefix}"
    local output="${PRIMARY_DIR}/${prefix}.out"
    local client_pid
    local backend
    local postmaster_pid

    PGAPPNAME="${app_name}" \
        psql -X -v ON_ERROR_STOP=1 -p "${PRIMARY_PORT}" -d "${TEST_DB}" \
        -c "
            SELECT injection_points_set_local();
            SELECT injection_points_attach(
                       'pg-textsearch-compaction-after-restamp', 'wait');
            SELECT bm25_compact_step('${prefix}_idx'::regclass);
        " >"${output}" 2>&1 &
    client_pid=$!
    backend=$(wait_for_backend "${app_name}")
    wait_for_injection \
        'pg-textsearch-compaction-after-restamp' "${backend}"

    postmaster_pid=$(head -1 "${PRIMARY_DIR}/postmaster.pid")
    pg_ctl stop -D "${PRIMARY_DIR}" -m immediate -w >/dev/null
    wait_for_postmaster_exit "${postmaster_pid}" "${prefix}"
    wait "${client_pid}" 2>/dev/null || true
    restart_primary
}

trigger_publication_panic() {
    local prefix=$1
    local point=$2
    local postmaster_pid
    local output
    local rc

    postmaster_pid=$(head -1 "${PRIMARY_DIR}/postmaster.pid")
    set +e
    output=$(timeout 30s psql -X -v ON_ERROR_STOP=1 \
        -p "${PRIMARY_PORT}" -d "${TEST_DB}" -c "
            SELECT pg_textsearch_test_attach_panic('${point}');
            SELECT bm25_compact_step('${prefix}_idx'::regclass);
        " 2>&1)
    rc=$?
    set -e

    [ "${rc}" -ne 0 ] ||
        error "${prefix}: compaction survived the ${point} panic"
    [ "${rc}" -ne 124 ] ||
        error "${prefix}: timed out waiting for PANIC: ${output}"
    wait_for_postmaster_exit "${postmaster_pid}" "${prefix}"
    log_contains "panic triggered for injection point ${point}" ||
        error "${prefix}: missing PANIC marker for ${point}"
    restart_primary
}

create_legacy_vacuum_fixture() {
    local prefix=$1
    local legacy_created

    primary_sql "
        SET pg_textsearch.memtable_pages_threshold = 0;
        SET pg_textsearch.bulk_load_threshold = 0;
        CREATE TABLE ${prefix}_docs (
            id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
            body text NOT NULL
        ) WITH (autovacuum_enabled = false);
        CREATE INDEX ${prefix}_idx
            ON ${prefix}_docs USING bm25(body)
            WITH (text_config = 'english', compaction = 'off');
        INSERT INTO ${prefix}_docs (body)
        SELECT 'legacy recovery document ' || gs || ' ' ||
               repeat(md5(gs::text), 4)
          FROM generate_series(1, 20000) gs;
    " >/dev/null

    primary_sql "
        SELECT pg_textsearch_test_attach_legacy_segment(1000000);
        SELECT bm25_spill_index('${prefix}_idx');
        SELECT injection_points_detach(
                   'pg-textsearch-legacy-segment');
    " >/dev/null
    legacy_created=$(primary_value "
        SELECT bm25_dump_index('${prefix}_idx') LIKE '%Version: 4%';")
    [ "${legacy_created}" = "t" ] ||
        error "${prefix}: legacy segment was not injected"
    primary_sql "
        DELETE FROM ${prefix}_docs WHERE id <= 5000;
        CHECKPOINT;
    " >/dev/null
}

legacy_root() {
    local prefix=$1

    primary_value "SELECT bm25_summarize_index('${prefix}_idx');" |
        sed -n 's/.*L0 Segment 1: block=\([0-9][0-9]*\).*/\1/p'
}

index_stat() {
    local prefix=$1
    local field=$2

    primary_value "SELECT bm25_summarize_index('${prefix}_idx');" |
        sed -n "s/^  ${field}: \\([0-9][0-9]*\\)$/\\1/p"
}

# The panic fires inside a parallel vacuum worker, so the attached
# condition matches the leader pid through the worker's lock group.
trigger_legacy_vacuum_panic() {
    local prefix=$1
    local point=$2
    local postmaster_pid
    local output
    local rc

    postmaster_pid=$(head -1 "${PRIMARY_DIR}/postmaster.pid")
    set +e
    output=$(PGOPTIONS="-c min_parallel_index_scan_size=0" \
        timeout 30s psql -X -v ON_ERROR_STOP=1 \
        -p "${PRIMARY_PORT}" -d "${TEST_DB}" \
        -c "SELECT pg_textsearch_test_attach_panic('${point}');" \
        -c "VACUUM (PARALLEL 1, VERBOSE) ${prefix}_docs;" 2>&1)
    rc=$?
    set -e

    [ "${rc}" -ne 0 ] ||
        error "${prefix}: legacy VACUUM survived the ${point} panic"
    [ "${rc}" -ne 124 ] ||
        error "${prefix}: timed out waiting for PANIC: ${output}"
    grep -q "launched 1 parallel vacuum worker" <<<"${output}" ||
        error "${prefix}: legacy VACUUM did not launch a parallel worker: ${output}"
    wait_for_postmaster_exit "${postmaster_pid}" "${prefix}"
    log_contains "panic triggered for injection point ${point}" ||
        error "${prefix}: missing PANIC marker for ${point}"
    restart_primary
}

assert_legacy_vacuum_recovery() {
    local prefix=$1
    local old_root=$2
    local expect_replaced=$3
    local expect_parked=$4
    local expected_total_docs=$5
    local expected_total_len=$6
    local heap_count
    local new_root
    local parked
    local plan
    local remaining
    local summary
    local total_docs
    local total_len

    new_root=$(legacy_root "${prefix}")
    [ -n "${new_root}" ] ||
        error "${prefix}: recovered graph has no L0 segment root"
    if [ "${expect_replaced}" = "yes" ]; then
        [ "${new_root}" != "${old_root}" ] ||
            error "${prefix}: legacy root ${old_root} was not replaced"
    else
        [ "${new_root}" = "${old_root}" ] ||
            error "${prefix}: unpublished replacement changed root to ${new_root}"
    fi

    parked=$(primary_value \
        "SELECT bm25_pending_free_pages('${prefix}_idx');")
    if [ "${expect_parked}" = "yes" ]; then
        [ "${parked}" -gt 0 ] ||
            error "${prefix}: replaced legacy root has no pending-free pages"
    else
        [ "${parked}" = "0" ] ||
            error "${prefix}: unpublished tombstones became reachable"
    fi

    remaining=$(primary_ranked_value "
        SELECT count(*)
          FROM (
                SELECT id
                  FROM ${prefix}_docs
                 ORDER BY body <@> to_bm25query(
                              'legacy recovery', '${prefix}_idx')
               ) ranked;")
    if [ "${remaining}" != "15000" ]; then
        heap_count=$(primary_value "SELECT count(*) FROM ${prefix}_docs;")
        plan=$(primary_ranked_value "
            EXPLAIN (COSTS off)
            SELECT id
              FROM ${prefix}_docs
             ORDER BY body <@> to_bm25query(
                          'legacy recovery', '${prefix}_idx');")
        summary=$(primary_value \
            "SELECT bm25_summarize_index('${prefix}_idx');")
        error "${prefix}: recovered ranked scan returned ${remaining}/15000 rows; heap=${heap_count}; plan=${plan}; summary=${summary}"
    fi

    summary=$(primary_value \
        "SELECT bm25_summarize_index('${prefix}_idx');")
    total_docs=$(sed -n 's/^  total_docs: \([0-9][0-9]*\)$/\1/p' \
        <<<"${summary}")
    total_len=$(sed -n 's/^  total_len: \([0-9][0-9]*\)$/\1/p' \
        <<<"${summary}")
    if [ "${expect_replaced}" = "no" ]; then
        [ "${total_docs}" = "${expected_total_docs}" ] ||
            error "${prefix}: recovered total_docs is ${total_docs}, expected ${expected_total_docs}"
        [ "${total_len}" = "${expected_total_len}" ] ||
            error "${prefix}: recovered total_len is ${total_len}, expected ${expected_total_len}"
    fi

    if [ "${expect_replaced}" = "yes" ]; then
        primary_value "SELECT txid_current();" >/dev/null
        primary_value "SELECT txid_current();" >/dev/null
        primary_sql "VACUUM ${prefix}_docs;" >/dev/null
        parked=$(primary_value \
            "SELECT bm25_pending_free_pages('${prefix}_idx');")
        [ "${parked}" -gt 0 ] ||
            error "${prefix}: provisional reclaim batch drained on activation"

        primary_value "SELECT txid_current();" >/dev/null
        primary_value "SELECT txid_current();" >/dev/null
        primary_sql "VACUUM ${prefix}_docs;" >/dev/null
        parked=$(primary_value \
            "SELECT bm25_pending_free_pages('${prefix}_idx');")
        [ "${parked}" = "0" ] ||
            error "${prefix}: activated reclaim batch remained parked (${parked})"

        primary_sql "REINDEX INDEX ${prefix}_idx;" >/dev/null
        expected_total_docs=$(index_stat "${prefix}" total_docs)
        expected_total_len=$(index_stat "${prefix}" total_len)
        [ "${total_docs}" = "${expected_total_docs}" ] ||
            error "${prefix}: recovered total_docs ${total_docs} differs from REINDEX ${expected_total_docs}"
        [ "${total_len}" = "${expected_total_len}" ] ||
            error "${prefix}: recovered total_len ${total_len} differs from REINDEX ${expected_total_len}"
    fi

    log "PASS: ${prefix} recovered root ${new_root}, pending ${parked}"
}

assert_crash_recovery() {
    local prefix=$1
    local expected_graph=$2
    local expected_segments=$3
    local expect_parked=$4
    local plan
    local expected_ids
    local actual_ids
    local graph
    local summary
    local parked

    plan=$(primary_ranked_value "
        EXPLAIN (COSTS off)
        SELECT id
          FROM ${prefix}_docs
         ORDER BY body <@> to_bm25query(
                      'crashmarker', '${prefix}_idx')
         LIMIT 600;")
    grep -Fq \
        "Index Scan using ${prefix}_idx on ${prefix}_docs" <<<"${plan}" ||
        error "${prefix}: recovery assertion did not use BM25 index: ${plan}"

    expected_ids=$(primary_value "
        SELECT string_agg(id::text, ',' ORDER BY id)
          FROM ${prefix}_docs;")
    actual_ids=$(primary_ranked_value "
        SELECT string_agg(id::text, ',' ORDER BY id)
          FROM (
                SELECT id
                  FROM ${prefix}_docs
                 ORDER BY body <@> to_bm25query(
                              'crashmarker', '${prefix}_idx')
                 LIMIT 600
               ) ranked;")
    [ "${actual_ids}" = "${expected_ids}" ] ||
        error "${prefix}: recovered ranked IDs differ from heap IDs ($(tr ',' '\n' <<<"${actual_ids}" | wc -l)/$(tr ',' '\n' <<<"${expected_ids}" | wc -l))"

    graph=$(primary_value \
        "SELECT bm25_level_counts('${prefix}_idx'::regclass)::text;")
    summary=$(primary_value \
        "SELECT bm25_summarize_index('${prefix}_idx');")
    [ "${graph}" = "${expected_graph}" ] ||
        error "${prefix}: graph is ${graph}, expected ${expected_graph}; ${summary}"
    [[ "${summary}" == *"Total: ${expected_segments} segments"* ]] ||
        error "${prefix}: invalid graph summary: ${summary}"

    parked=$(primary_value \
        "SELECT bm25_pending_free_pages('${prefix}_idx');")
    case "${parked}" in
        ''|*[!0-9]*) error "${prefix}: invalid pending count '${parked}'" ;;
    esac
    if [ "${expect_parked}" = "yes" ]; then
        [ "${parked}" -gt 0 ] ||
            error "${prefix}: published graph has no pending-free chain"
    else
        [ "${parked}" = "0" ] ||
            error "${prefix}: unpublished tombstones became reachable"
    fi

    log "PASS: ${prefix} recovered IDs, graph ${graph}, pending ${parked}"
}

test_publication_crash_recovery() {
    if [ "${HAS_INJECTION_POINTS}" -ne 1 ]; then
        log "Skipping publication crash recovery: no injection points"
        return
    fi

    log "Testing compaction publication crash recovery..."

    create_crash_fixture detached_wal
    crash_after_detached_wal detached_wal
    assert_crash_recovery \
        detached_wal "{2,0,0,0,0,0,0,0}" 2 no

    create_crash_fixture before_publish
    trigger_publication_panic \
        before_publish \
        pg-textsearch-before-compaction-publish
    assert_crash_recovery \
        before_publish "{2,0,0,0,0,0,0,0}" 2 no

    create_crash_fixture after_publish
    trigger_publication_panic \
        after_publish \
        pg-textsearch-after-compaction-publish
    assert_crash_recovery \
        after_publish "{0,1,0,0,0,0,0,0}" 1 yes
}

test_legacy_vacuum_crash_recovery() {
    local old_root
    local old_total_docs
    local old_total_len

    if [ "${HAS_INJECTION_POINTS}" -ne 1 ]; then
        log "Skipping legacy VACUUM crash recovery: no injection points"
        return
    fi

    log "Testing parallel legacy VACUUM publication crash recovery..."

    create_legacy_vacuum_fixture legacy_before_publish
    old_root=$(legacy_root legacy_before_publish)
    old_total_docs=$(index_stat legacy_before_publish total_docs)
    old_total_len=$(index_stat legacy_before_publish total_len)
    [ -n "${old_root}" ] ||
        error "legacy_before_publish: fixture has no L0 segment root"
    trigger_legacy_vacuum_panic \
        legacy_before_publish \
        pg-textsearch-before-compaction-publish
    assert_legacy_vacuum_recovery \
        legacy_before_publish "${old_root}" no no \
        "${old_total_docs}" "${old_total_len}"

    create_legacy_vacuum_fixture legacy_after_publish
    old_root=$(legacy_root legacy_after_publish)
    [ -n "${old_root}" ] ||
        error "legacy_after_publish: fixture has no L0 segment root"
    trigger_legacy_vacuum_panic \
        legacy_after_publish \
        pg-textsearch-after-compaction-publish
    assert_legacy_vacuum_recovery \
        legacy_after_publish "${old_root}" yes yes "" ""
}

expect_recovery_rejection() {
    local function_name=$1
    local output
    local expected_message="cannot compact a bm25 index during recovery"

    if output=$(psql -v ON_ERROR_STOP=1 -v VERBOSITY=verbose \
        -p "${STANDBY_PORT}" -d "${TEST_DB}" \
        -c "SELECT ${function_name}(
                'compaction_recovery_idx'::regclass);" 2>&1); then
        error "${function_name} unexpectedly succeeded during recovery"
    fi

    if [[ "${output}" != *"25006"* ]] ||
       [[ "${output}" != *"${expected_message}"* ]]; then
        error "${function_name} returned the wrong recovery error: ${output}"
    fi

    log "PASS: ${function_name} rejected execution during recovery"
}

test_standby_guards() {
    primary_sql "
        CREATE TABLE compaction_recovery (
            id integer PRIMARY KEY,
            body text
        );
        INSERT INTO compaction_recovery
        VALUES (1, 'standby compaction guard');
        CREATE INDEX compaction_recovery_idx
            ON compaction_recovery USING bm25(body)
            WITH (text_config = 'english');
    " >/dev/null

    setup_standby
    wait_for_standby_catchup
    expect_recovery_rejection bm25_compact
    expect_recovery_rejection bm25_compact_step
}

main() {
    log "Starting compaction recovery tests..."
    check_required_tools
    setup_primary
    cat >>"${PRIMARY_DIR}/postgresql.conf" <<EOF
restart_after_crash = off
log_min_messages = log
pg_textsearch.segments_per_level = 2
EOF
    pg_ctl restart -D "${PRIMARY_DIR}" \
        -l "${PRIMARY_DIR}/postgres.log" -m fast -w

    test_publication_crash_recovery
    test_legacy_vacuum_crash_recovery
    test_standby_guards
    log "Compaction recovery tests PASSED"
}

main "$@"
