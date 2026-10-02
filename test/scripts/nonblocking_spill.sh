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
POINT_BEFORE_TRUNCATE='pg-textsearch-force-merge-before-truncate'
POINT_AFTER_INSPECTION='pg-textsearch-truncate-after-inspection'

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
    local actual

    while ((SECONDS < deadline)); do
        if [ "$(sql -c "
            SELECT wait_event
            FROM pg_stat_activity
            WHERE pid = ${backend};")" = "${wait_event}" ]; then
            return
        fi
        sleep 0.05
    done
    actual=$(sql -c "
        SELECT wait_event_type || ':' || wait_event
        FROM pg_stat_activity WHERE pid = ${backend};")
    fail "backend ${backend} did not wait on ${wait_event} (got ${actual})"
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

cancel_blocks_after_error=$(sql -c "
    SELECT pg_relation_size('spill_cancel_idx'::regclass, 'main') /
           current_setting('block_size')::integer;")
sql -c "SELECT bm25_spill_index('spill_cancel_idx');" >/dev/null
cancel_graph=$(sql -c "
    SELECT bm25_level_counts('spill_cancel_idx'::regclass)::text;")
cancel_blocks_after_retry=$(sql -c "
    SELECT pg_relation_size('spill_cancel_idx'::regclass, 'main') /
           current_setting('block_size')::integer;")
[ "${cancel_graph}" = "{1,0,0,0,0,0,0,0}" ] ||
    fail "spill after cancellation did not publish one segment"
[ "${cancel_blocks_after_retry}" = "${cancel_blocks_after_error}" ] ||
    fail "spill retry extended from ${cancel_blocks_after_error} to ${cancel_blocks_after_retry} blocks instead of reusing discarded output"

sql -c "
    CREATE TABLE published_docs (id integer PRIMARY KEY, body text NOT NULL);
    CREATE INDEX published_idx ON published_docs USING bm25(body)
        WITH (text_config = 'english', compaction = 'manual');
    INSERT INTO published_docs
    SELECT gs, 'publishedterm document ' || gs
    FROM generate_series(1, 500) gs;" >/dev/null

if sql -c "
    SELECT injection_points_set_local();
    SELECT injection_points_attach(
        'pg-textsearch-after-spill-finalize', 'error');
    SELECT bm25_spill_index('published_idx');" \
    >"${CLIENT_DIR}/published_error.log" 2>&1; then
    fail "post-publication error did not fire"
fi
grep -q 'error triggered for injection point' \
    "${CLIENT_DIR}/published_error.log" ||
    fail "post-publication spill failed for an unexpected reason"

published_graph=$(sql -c "
    SELECT bm25_level_counts('published_idx'::regclass)::text;")
[ "${published_graph}" = "{1,0,0,0,0,0,0,0}" ] ||
    fail "post-publication error lost the segment"
[ "$(sql -c "
    SELECT COALESCE(sum(n_records), 0)
    FROM bm25_memtable_chain('published_idx');")" = "0" ] ||
    fail "post-publication error left the old chain published"
published_ids=$(sql -c "
    SET enable_seqscan = off;
    SELECT string_agg(id::text, ',' ORDER BY id)
    FROM (
        SELECT id FROM published_docs
        ORDER BY body <@> to_bm25query('publishedterm', 'published_idx')
        LIMIT 500
    ) ranked;")
[ "${published_ids}" = "$(seq -s, 1 500)" ] ||
    fail "post-publication error damaged the published segment"

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

sql -c "
    CREATE EXTENSION pg_textsearch_test;
    CREATE TABLE capacity_docs (id integer, body text);
    CREATE INDEX capacity_idx ON capacity_docs USING bm25(body)
        WITH (text_config = 'english', compaction = 'manual');
    INSERT INTO capacity_docs VALUES (0, 'capacityterm');
    SELECT bm25_spill_index('capacity_idx');
    INSERT INTO capacity_docs
    SELECT gs, 'capacityterm document ' || gs
    FROM generate_series(1, 500) gs;
    SELECT pg_textsearch_test_attach_segment_limit(1);
    DO \$\$
    DECLARE
        before_bytes bigint := pg_relation_size('capacity_idx');
    BEGIN
        BEGIN
            PERFORM bm25_spill_index('capacity_idx');
            RAISE EXCEPTION 'full L0 spill unexpectedly succeeded';
        EXCEPTION WHEN program_limit_exceeded THEN
            NULL;
        END;
        IF pg_relation_size('capacity_idx') <> before_bytes THEN
            RAISE EXCEPTION 'full L0 spill allocated unreachable output';
        END IF;
    END
    \$\$;
    SELECT injection_points_detach(
        'pg-textsearch-segment-count-limit');" >/dev/null

# A shutdown spill must not enter the blocking acquisition at either phase.
for phase in freeze publish; do
    sql -c "
        CREATE TABLE shutdown_${phase}_docs (id integer, body text);
        CREATE INDEX shutdown_${phase}_idx
            ON shutdown_${phase}_docs USING bm25(body)
            WITH (text_config = 'english', compaction = 'manual');
        INSERT INTO shutdown_${phase}_docs
        SELECT gs, 'shutdownterm document ' || gs
        FROM generate_series(1, 500) gs;" >/dev/null

    point='pg-textsearch-index-lock-exclusive-waiter'
    if [ "${phase}" = publish ]; then
        point="${POINT_SPILL_BEFORE_FINALIZE}"
    fi
    PGAPPNAME=shutdown-target sql -c "
        SELECT count(*) FROM (
            SELECT 1 FROM shutdown_${phase}_docs
            ORDER BY body <@> to_bm25query(
                'shutdownterm', 'shutdown_${phase}_idx')
            LIMIT 1
        ) ranked;
        SELECT pg_sleep(60);" >"${CLIENT_DIR}/shutdown_${phase}.log" 2>&1 &
    shutdown_client=$!
    shutdown_backend=$(backend_pid shutdown-target)
    wait_for_wait_event "${shutdown_backend}" PgSleep
    sql -c "SELECT injection_points_attach('${point}', 'wait');" >/dev/null

    sql -c "SELECT pg_terminate_backend(${shutdown_backend}, 5000);" \
        >"${CLIENT_DIR}/terminate_${phase}.log" 2>&1 &
    terminate_client=$!
    if [ "${phase}" = publish ]; then
        wait_for_injection "${shutdown_backend}"
        sql -c "
            SELECT injection_points_attach(
                'pg-textsearch-index-lock-exclusive-waiter', 'wait');
            SELECT injection_points_wakeup(
                '${POINT_SPILL_BEFORE_FINALIZE}');" >/dev/null
    fi
    wait_for_exit "${terminate_client}" 7 "shutdown ${phase} termination"
    grep -qx 't' "${CLIENT_DIR}/terminate_${phase}.log" ||
        fail "shutdown ${phase} used a blocking index acquisition"
    if wait "${shutdown_client}"; then
        fail "terminated shutdown ${phase} client unexpectedly succeeded"
    fi
    sql -c "SELECT injection_points_detach('${point}');" >/dev/null
    if [ "${phase}" = publish ]; then
        sql -c "SELECT injection_points_detach(
            'pg-textsearch-index-lock-exclusive-waiter');" >/dev/null
    fi
    [ "$(sql -c "
        SELECT bm25_level_counts('shutdown_${phase}_idx'::regclass)::text;
    ")" = "{1,0,0,0,0,0,0,0}" ] ||
        fail "shutdown ${phase} did not publish its uncontended spill"
done

# Removing the truncator's writer gate must fail both lock-order checks.
# VACUUM leaves a large recyclable suffix behind a small surviving segment.
for ordering in spill_first truncate_first; do
    sql -c "
        CREATE TABLE ${ordering}_docs (id integer PRIMARY KEY, body text);
        INSERT INTO ${ordering}_docs
        SELECT gs, 'truncateterm original ' || gs
        FROM generate_series(1, 500) gs;
        CREATE INDEX ${ordering}_idx ON ${ordering}_docs USING bm25(body)
            WITH (text_config = 'english', compaction = 'manual');
        INSERT INTO ${ordering}_docs
        SELECT gs, 'discardterm ' || md5(gs::text)
        FROM generate_series(1001, 11000) gs;
        SELECT bm25_spill_index('${ordering}_idx');
        DELETE FROM ${ordering}_docs WHERE id > 1000;" >/dev/null
    sql -c "VACUUM ${ordering}_docs;" >/dev/null
    sql -c "SELECT txid_current();" >/dev/null
    sql -c "SELECT txid_current();" >/dev/null
    sql -c "VACUUM ${ordering}_docs;" >/dev/null
    [ "$(sql -c "
        SELECT bm25_pending_free_pages('${ordering}_idx');")" = "0" ] ||
        fail "${ordering}: fixture still has parked pages"

    PGAPPNAME=truncate-merge sql -c "
        SET statement_timeout = '60s';
        SELECT injection_points_set_local();
        SELECT injection_points_attach('${POINT_BEFORE_TRUNCATE}', 'wait');
        SELECT injection_points_attach('${POINT_AFTER_INSPECTION}', 'wait');
        SELECT bm25_force_merge('${ordering}_idx');" \
        >"${CLIENT_DIR}/${ordering}_merge.log" 2>&1 &
    merge_client=$!
    merge_backend=$(backend_pid truncate-merge)
    wait_for_wait_event "${merge_backend}" "${POINT_BEFORE_TRUNCATE}"

    # The force-merge's initial spill is already over. Add a fresh chain
    # without letting a session-exit spill consume it.
    sql -c "
        INSERT INTO ${ordering}_docs
        SELECT gs, 'truncateterm pending ' || gs
        FROM generate_series(501, 1000) gs;
        CREATE TABLE ${ordering}_expected AS
        SELECT id, body <@> to_bm25query(
            'truncateterm', '${ordering}_idx') AS score
        FROM ${ordering}_docs
        ORDER BY body <@> to_bm25query('truncateterm', '${ordering}_idx')
        LIMIT 1000;" >/dev/null
    bytes_before=$(sql -c "
        SELECT pg_relation_size('${ordering}_idx');")

    if [ "${ordering}" = truncate_first ]; then
        sql -c "SELECT injection_points_wakeup(
            '${POINT_BEFORE_TRUNCATE}');" >/dev/null
        wait_for_wait_event "${merge_backend}" "${POINT_AFTER_INSPECTION}"
    fi

    PGAPPNAME=truncate-spill sql -c "
        SET statement_timeout = '60s';
        SELECT injection_points_set_local();
        SELECT injection_points_attach(
            '${POINT_SPILL_BEFORE_FINALIZE}', 'wait');
        SELECT bm25_spill_index('${ordering}_idx');" \
        >"${CLIENT_DIR}/${ordering}_spill.log" 2>&1 &
    spill_client=$!
    spill_backend=$(backend_pid truncate-spill)

    if [ "${ordering}" = spill_first ]; then
        wait_for_injection "${spill_backend}"
        # Construction has released the index lock, but not the writer
        # gate. Truncation must wait for that in-flight allocation owner.
        sql -c "SELECT injection_points_wakeup(
            '${POINT_BEFORE_TRUNCATE}');" >/dev/null
        wait_for_wait_event "${merge_backend}" tapir_memtable_write_lock
        sql -c "SELECT injection_points_wakeup(
            '${POINT_SPILL_BEFORE_FINALIZE}');" >/dev/null
        wait_for_exit "${spill_client}" 10 "spill before truncation"
        wait_for_wait_event "${merge_backend}" "${POINT_AFTER_INSPECTION}"
        sql -c "SELECT injection_points_wakeup(
            '${POINT_AFTER_INSPECTION}');" >/dev/null
        wait_for_exit "${merge_client}" 10 "merge after spill"
    else
        # An inspected suffix is still in the FSM. No spill may start
        # allocating it before RelationTruncate completes.
        wait_for_wait_event "${spill_backend}" tapir_memtable_write_lock
        sql -c "SELECT injection_points_wakeup(
            '${POINT_AFTER_INSPECTION}');" >/dev/null
        wait_for_exit "${merge_client}" 10 "merge before spill"
        wait_for_injection "${spill_backend}"
        sql -c "SELECT injection_points_wakeup(
            '${POINT_SPILL_BEFORE_FINALIZE}');" >/dev/null
        wait_for_exit "${spill_client}" 10 "spill after truncation"
    fi

    bytes_after=$(sql -c "
        SELECT pg_relation_size('${ordering}_idx');")
    [ "${bytes_after}" -lt "${bytes_before}" ] ||
        fail "${ordering}: fixture did not exercise suffix truncation"
done

# Read every segment page map (including root/page-index references), then
# compare every ranked ID and score with the pre-spill chain result.
check_truncated_indexes() {
    local ordering

    for ordering in spill_first truncate_first; do
        sql -c "
            SET enable_seqscan = off;
            DO \$\$
            DECLARE
                summary text := bm25_summarize_index('${ordering}_idx');
            BEGIN
                IF summary !~ 'docs_persisted: 1000'
                   OR (SELECT COALESCE(sum(n_records), 0)
                       FROM bm25_memtable_chain('${ordering}_idx')) <> 0
                THEN
                    RAISE EXCEPTION 'invalid post-truncate graph: %', summary;
                END IF;
                IF EXISTS (
                    WITH actual AS (
                        SELECT id, body <@> to_bm25query(
                            'truncateterm', '${ordering}_idx') AS score
                        FROM ${ordering}_docs
                        ORDER BY body <@> to_bm25query(
                            'truncateterm', '${ordering}_idx')
                        LIMIT 1000
                    )
                    (SELECT * FROM actual
                     EXCEPT ALL SELECT * FROM ${ordering}_expected)
                    UNION ALL
                    (SELECT * FROM ${ordering}_expected
                     EXCEPT ALL SELECT * FROM actual)
                ) THEN
                    RAISE EXCEPTION 'truncation changed ranked results';
                END IF;
                IF (SELECT count(*) FROM ${ordering}_expected) <> 1000 THEN
                    RAISE EXCEPTION 'incomplete pre-spill query result';
                END IF;
            END
            \$\$;" >/dev/null || fail "${ordering}: graph/result check failed"
    done
}

check_truncated_indexes
# Spill is a physical change in a read-only SQL transaction. Force a later
# commit record to flush its WAL before testing recovery of the new graph.
sql -c "
    UPDATE truncate_first_expected SET score = score WHERE id = 1;" \
    >/dev/null
pg_ctl stop -D "${DATA_DIR}" -m immediate -w >/dev/null
pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w >/dev/null ||
    fail "PostgreSQL recovery startup failed"
check_truncated_indexes

echo "nonblocking spill test passed"
