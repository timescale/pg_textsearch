#!/bin/bash
# Verify alive-count consistency after a crash between bitmap page writes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PG_CONFIG="${PG_CONFIG:-pg_config}"
PGBINDIR="$("${PG_CONFIG}" --bindir)"
export PATH="${PGBINDIR}:${PATH}"
PRIMARY_PORT=55466
STANDBY_PORT=55467
TEST_DB=alive_bitset_recovery_test
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_alive_bitset_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_alive_bitset_standby"
REPL_SOCKET_ONLY=1

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

if [ "${HAS_INJECTION_POINTS}" -ne 1 ]; then
    log 'Skipping alive bitset crash recovery: no injection points'
    exit 0
fi

PANIC_POINT=pg-textsearch-alive-bitset-after-page

sql() {
    psql -X -v ON_ERROR_STOP=1 -p "${PRIMARY_PORT}" -d "${TEST_DB}" \
        -At -c "$1"
}

assert_alive() {
    local expected=$1
    local dump
    dump=$(sql "SELECT bm25_dump_index('docs_idx');")
    if ! grep -Fq "Alive: ${expected} / ${num_docs} docs" <<< "${dump}"; then
        echo "${dump}" >&2
        error "expected ${expected} live documents in the original segment"
    fi
}

check_topk() {
    local stage=$1
    local query="SELECT id, body,
        body <@> to_bm25query('needle', 'docs_idx') AS score
        FROM docs
        ORDER BY body <@> to_bm25query('needle', 'docs_idx') LIMIT 10"
    local index_settings="SET enable_seqscan=off;"
    local seq_settings="SET enable_seqscan=on; SET enable_indexscan=off;
        SET enable_bitmapscan=off;"
    local index_plan seq_plan scores oracle rows

    index_plan=$(sql "${index_settings} EXPLAIN ${query};")
    seq_plan=$(sql "${seq_settings} EXPLAIN ${query};")
    [[ "${index_plan}" == *"Index Scan using docs_idx"* ]] ||
        error "${stage}: expected BM25 index scan: ${index_plan}"
    [[ "${seq_plan}" == *"Seq Scan on docs"* &&
       "${seq_plan}" == *"Sort"* ]] ||
        error "${stage}: expected SeqScan + Sort: ${seq_plan}"

    # All matching rows have the same text. Compare with the sequential
    # oracle so the check follows the scoring model and allows tied IDs.
    scores=$(sql "${index_settings} ${query};" | grep '|')
    oracle=$(sql "${seq_settings} ${query};" | grep '|')
    for rows in "${scores}" "${oracle}"; do
        awk -F '|' '
            $2 != "needle" { bad = 1 }
            END { exit (bad || NR != 10) }
        ' <<< "${rows}" || error "${stage}: incorrect Top-K: ${rows}"
    done
    diff -u <(cut -d '|' -f 2- <<< "${scores}") \
        <(cut -d '|' -f 2- <<< "${oracle}") ||
        error "${stage}: index scores differ from the sequential oracle"
    log "PASS: ${stage} Top-K agrees with SeqScan + Sort"
}

crash_during_vacuum() {
    local postmaster_pid output rc
    local deadline=$((SECONDS + 30))

    postmaster_pid=$(head -1 "${PRIMARY_DIR}/postmaster.pid")
    set +e
    output=$(timeout 30s psql -X -v ON_ERROR_STOP=1 \
        -p "${PRIMARY_PORT}" -d "${TEST_DB}" \
        -c "SELECT pg_textsearch_test_attach_panic('${PANIC_POINT}');" \
        -c 'VACUUM (INDEX_CLEANUP ON, PARALLEL 0) docs;' 2>&1)
    rc=$?
    set -e
    [[ "${rc}" -ne 0 && "${rc}" -ne 124 ]] ||
        error "VACUUM did not reach the injected PANIC: ${output}"
    while kill -0 "${postmaster_pid}" 2>/dev/null; do
        (( SECONDS < deadline )) || error 'postmaster did not exit'
        sleep 0.05
    done
    grep -Fq "panic triggered for injection point ${PANIC_POINT}" \
        "${PRIMARY_DIR}/log/postgres.log" ||
        error "missing bitmap-page PANIC: ${output}"

    # Recover the original data directory through the end of its WAL.
    pg_ctl start -D "${PRIMARY_DIR}" -l "${PRIMARY_DIR}/postgres.log" -w \
        >/dev/null
    [[ "$(sql 'SELECT pg_is_in_recovery();')" == f ]] ||
        error 'crash recovery did not complete'
}

run_case() {
    local label=$1
    local delete_tail=$2
    local boundary_case=${3:-0}
    local first_page_deleted=8
    local boundary_clause=false
    local bitset_offset boundary
    local deleted=$((8 + delete_tail))
    local final_alive=$((num_docs - deleted))
    local reused

    log "Testing ${label}..."
    sql "SET max_parallel_maintenance_workers=0;
        SET synchronize_seqscans=off;
        SET pg_textsearch.memtable_pages_threshold=0;
        SET pg_textsearch.bulk_load_threshold=0;
        CREATE TABLE docs(id integer, body text)
            WITH (autovacuum_enabled=false);
        INSERT INTO docs SELECT i, 'needle'
            FROM generate_series(1,${num_docs}) i;
        CREATE INDEX docs_idx ON docs USING bm25(body)
            WITH (text_config='simple', compaction='off');
        CREATE TABLE deleted_tids AS
            SELECT ctid AS tid FROM docs WHERE id<=8;
        CHECKPOINT;" >/dev/null
    assert_alive "${num_docs}"

    if [[ "${boundary_case}" -eq 1 ]]; then
        bitset_offset=$(sql "SELECT bm25_dump_index('docs_idx');" |
            sed -n 's/.*Alive bitset offset: \([0-9]*\).*/\1/p')
        [[ "${bitset_offset}" =~ ^[0-9]+$ ]] ||
            error 'expected one segment with an alive bitmap offset'
        # PostgreSQL's fixed page header (SizeOfPageHeaderData) is 24 bytes.
        # Serial build assigns doc IDs in heap order, starting at zero.
        local page_data=$((block_size - 24))
        [[ $((bitset_offset % page_data)) -ne 0 ]] ||
            error 'boundary fixture needs a nonzero bitmap page offset'
        boundary=$(((page_data - bitset_offset % page_data) * 8))
        [[ "${boundary}" -gt 16 && "${boundary}" -lt $((num_docs - 16)) ]] ||
            error 'bitmap boundary overlaps another deletion range'
        boundary_clause="id BETWEEN ${boundary}-3 AND ${boundary}+4"
        first_page_deleted=$((first_page_deleted + 4))
        deleted=$((deleted + 8))
        final_alive=$((num_docs - deleted))
    fi

    # The PANIC callback flushes the first page's WAL record, then crashes
    # before the writer can generate any later bitmap-page records.
    sql "DELETE FROM docs WHERE id<=8 OR id>${num_docs}-${delete_tail}
        OR (${boundary_clause});" \
        >/dev/null
    crash_during_vacuum
    assert_alive "$((num_docs - first_page_deleted))"
    log 'PASS: crash recovery preserves the first bitmap page and its count'

    # A second startup must see the same completed recovery state.
    pg_ctl restart -D "${PRIMARY_DIR}" -l "${PRIMARY_DIR}/postgres.log" \
        -m fast -w >/dev/null
    assert_alive "$((num_docs - first_page_deleted))"
    for attempt in 1 2; do
        sql 'VACUUM (INDEX_CLEANUP ON) docs;' >/dev/null
        assert_alive "${final_alive}"
    done
    log 'PASS: VACUUM completes partial progress and is idempotent'
    [[ "$(sql 'SELECT count(*) FROM docs;')" == "${final_alive}" ]] ||
        error 'unexpected heap row count after recovery'
    # A later VACUUM must count fresh deaths without subtracting old ones.
    sql 'DELETE FROM docs WHERE id BETWEEN 9 AND 12;' >/dev/null
    deleted=$((deleted + 4))
    final_alive=$((final_alive - 4))
    for attempt in 1 2; do
        sql 'VACUUM (INDEX_CLEANUP ON) docs;' >/dev/null
        assert_alive "${final_alive}"
    done
    log 'PASS: a later deletion batch updates the count exactly once'
    sql "SET pg_textsearch.memtable_pages_threshold=0;
        INSERT INTO docs SELECT i, 'other'
        FROM generate_series(${num_docs}+1,${num_docs}+${deleted}) i;" \
        >/dev/null
    reused=$(sql "SELECT count(*) FROM docs d JOIN deleted_tids t ON d.ctid=t.tid
        WHERE d.id>${num_docs};")
    [[ "${reused}" -gt 0 ]] || error 'fixture did not reuse any first-page TIDs'
    log "PASS: new rows reused ${reused} TIDs from the first bitmap page"
    check_topk "${label}: before merge"
    sql "SELECT bm25_force_merge('docs_idx');" >/dev/null
    check_topk "${label}: after merge"
    sql 'DROP TABLE docs, deleted_tids;' >/dev/null
}

setup_primary
cat >> "${PRIMARY_DIR}/postgresql.conf" <<EOF
restart_after_crash = off
EOF
pg_ctl restart -D "${PRIMARY_DIR}" -l "${PRIMARY_DIR}/postgres.log" \
    -m fast -w >/dev/null
# One bit per document: exceed a whole physical page by one byte so the
# bitmap must span pages regardless of its starting offset or BLCKSZ.
block_size=$(sql 'SHOW block_size;')
num_docs=$((block_size * 8 + 8))
run_case 'deletions on the first page' 0
run_case 'deletions on the first and last pages' 8
run_case 'deletions across a bitmap page boundary' 8 1
log 'Alive bitset crash recovery tests PASSED'
