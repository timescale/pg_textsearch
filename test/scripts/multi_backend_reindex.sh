#!/bin/bash
#
# multi_backend_reindex.sh — regression test for issue #390:
#   "bug: BM25 index returns stale heap TIDs after heap rewrite"
#
# Reproduces the multi-backend scenario in which one backend has already
# touched the BM25 index (priming its private TpLocalIndexState cache)
# when a second backend runs a statement that gives the index a new
# relfilenode and rebuilds it from scratch. Several distinct DDL paths
# trigger this:
#
#   * ALTER TABLE ... ADD COLUMN ... GENERATED ALWAYS AS (...) STORED
#   * VACUUM FULL <table>
#   * CLUSTER <table> USING <pkey>
#   * REINDEX INDEX <bm25_index>   (index relfilenode only; heap intact)
#
# On v1.2.0 (in-memory memtable in DSA) the first backend's
# local_state_cache (`src/index/state.c`, `tp_get_local_index_state`)
# was never invalidated because pg_textsearch registers no
# `CacheRegisterRelcacheCallback`. Subsequent writes from that backend
# went to the OLD memtable still living in DSA, and a later spill
# wrote a segment of OLD (now stale) heap CTIDs into the NEW index
# file. Queries returning those stale CTIDs then failed with:
#
#   ERROR: could not read blocks N..N in file "base/<db>/<heap_fno>":
#          read only 0 of 8192 bytes
#
# As of pg_textsearch 1.3.0 (#374/#375/#392) the memtable lives in
# the index relation's own on-disk pages, so the new relfilenode is
# automatically empty. The shared-state allocation and its LWLock
# must remain stable across rewrites because backend-local wrappers
# are intentionally cached for the backend lifetime. This test pins
# both properties for every heap-rewrite / index-rebuild path.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKTREE_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# Allow override but default to a port not used by any other test script.
# (cross-checked against test/scripts/*.sh: 55434–55450, 55454–55457,
# 55460–55462 are taken; 55458 is free as of this writing.)
TEST_PORT="${TEST_PORT:-55458}"
TEST_DB=pg_textsearch_reindex_test
# Unique per-PID data dir so concurrent invocations (e.g. accidental
# parallel make) cannot rm -rf each other's clusters.
DATA_DIR="${SCRIPT_DIR}/../tmp_reindex_test_${TEST_PORT}_$$"
SOCKET_DIR="${WORKTREE_ROOT}/.s$$"
LOGFILE="${DATA_DIR}/postgres.log"
# Preserved server-log destination if the test fails — kept outside
# DATA_DIR so it survives the cleanup `rm -rf`. CI can `cat` this on
# failure for post-mortem.
PRESERVED_LOGFILE="${SCRIPT_DIR}/../tmp_reindex_test_${TEST_PORT}_$$_postgres.log"
PREPARE_CLIENT_PID=

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"; }
warn() { echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"; }
error() { echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"; exit 1; }
info() { echo -e "${BLUE}[$(date '+%H:%M:%S')] $1${NC}"; }

cleanup() {
    local exit_code=$?
    # Disarm the trap so a signal during cleanup doesn't re-enter us.
    trap - EXIT INT TERM
    log "Cleaning up reindex test environment (exit code: $exit_code)..."
    stop_prepare_client || true
    # On failure, preserve postgres.log outside DATA_DIR so CI can
    # surface it for debugging — DATA_DIR is about to be wiped.
    if [ "${exit_code}" -ne 0 ] && [ -f "${LOGFILE}" ]; then
        cp "${LOGFILE}" "${PRESERVED_LOGFILE}" 2>/dev/null || true
        if [ -f "${PRESERVED_LOGFILE}" ]; then
            warn "Preserved server log: ${PRESERVED_LOGFILE}"
        fi
    fi
    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m fast -w &>/dev/null ||
            pg_ctl stop -D "${DATA_DIR}" -m immediate -w &>/dev/null ||
            true
    fi
    rm -rf "${DATA_DIR}"
    rm -rf "${SOCKET_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up reindex test PostgreSQL instance..."

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"
    mkdir -p "${SOCKET_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1

    # Pin spill thresholds so the only memtable spill in this test is
    # the one we force via bm25_spill_index() — guards against future
    # default changes that might cause an implicit spill before the
    # rewrite, hiding the bug being exercised.
    # Both GUCs accept 0 as "disable": tp_bulk_load_threshold short-
    # circuits at src/index/state.c when <= 0, and the page-threshold
    # auto-spill returns early when == 0 at src/access/build.c.
    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 20
max_worker_processes = 8
max_parallel_workers = 4
max_parallel_maintenance_workers = 2
max_prepared_transactions = 10
shared_buffers = 128MB
unix_socket_directories = '${SOCKET_DIR}'
listen_addresses = ''
log_min_messages = warning
shared_preload_libraries = 'pg_textsearch'
pg_textsearch.bulk_load_threshold = 0
pg_textsearch.memtable_pages_threshold = 0
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w \
        || error "Failed to start PostgreSQL"

    createdb -h "${SOCKET_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

run_sql() {
    psql -X -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "$1" 2>&1
}

run_sql_quiet() {
    psql -X -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -c "$1" >/dev/null 2>&1
}

# run_sql_value: scalar fetch. Captures stderr (2>&1) so that any
# server-side ERROR surfaces in the returned string and downstream
# regex checks can detect storage corruption. Numeric callers must
# validate the format (e.g. `[[ $val =~ ^[0-9]+$ ]]`) because the
# returned value may be an error message rather than a number.
run_sql_value() {
    psql -X -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -tAc "$1" 2>&1
}

run_sql_quiet_bounded() {
    timeout --signal=TERM --kill-after=2s 20s \
        psql -X -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -q -c "$1" >/dev/null 2>&1
}

run_sql_value_bounded() {
    timeout --signal=TERM --kill-after=2s 20s \
        psql -X -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" -tAc "$1" 2>&1
}

prepare_client_is_running() {
    local state

    [ -n "${PREPARE_CLIENT_PID}" ] || return 1
    state=$(ps -o stat= -p "${PREPARE_CLIENT_PID}" 2>/dev/null) || return 1
    [[ "${state}" != Z* ]]
}

wait_for_prepare_client_exit() {
    local deadline=$((SECONDS + 10))
    local client_pid="${PREPARE_CLIENT_PID}"

    [ -n "${client_pid}" ] || return 0

    while prepare_client_is_running; do
        if [ "${SECONDS}" -ge "${deadline}" ]; then
            return 1
        fi
        sleep 0.1
    done

    wait "${client_pid}" 2>/dev/null || true
    PREPARE_CLIENT_PID=
}

stop_prepare_client() {
    local client_pid="${PREPARE_CLIENT_PID}"

    [ -n "${client_pid}" ] || return 0

    kill "${client_pid}" 2>/dev/null || true
    if wait_for_prepare_client_exit; then
        return 0
    fi

    kill -KILL "${client_pid}" 2>/dev/null || true
    wait "${client_pid}" 2>/dev/null || true
    PREPARE_CLIENT_PID=
    return 1
}

wait_for_prepare_marker() {
    local marker_file="$1"
    local deadline=$((SECONDS + 10))

    while [ ! -f "${marker_file}" ]; do
        if ! prepare_client_is_running; then
            return 1
        fi
        if [ "${SECONDS}" -ge "${deadline}" ]; then
            return 1
        fi
        sleep 0.05
    done
}

# Build a committed index whose old relfilenode has no memtable chain.  Each
# rollback test then REINDEXes it, appends enough large records to put the new
# cache cursor beyond the old file's EOF, and opens the cache through a normal
# BM25 query.  PostgreSQL restores the old relfilenode at the selected rollback
# boundary, so the next normal query must reject the discarded-file cursor.
prepare_cache_locator_rollback_fixture() {
    run_sql_quiet "
        DROP TABLE IF EXISTS cache_locator_docs CASCADE;
        CREATE TABLE cache_locator_docs (
            id      bigint PRIMARY KEY,
            content text NOT NULL
        );
        INSERT INTO cache_locator_docs
        SELECT g, 'locatoranchor committed row ' || g
        FROM generate_series(1, 20) g;
        CREATE INDEX cache_locator_idx ON cache_locator_docs
        USING bm25 (content) WITH (text_config='english');
    "
}

cache_locator_rollback_insert_sql() {
    cat << 'EOF'
INSERT INTO cache_locator_docs
SELECT 1000 + g,
       'locatoranchor rollbackonly payload ' ||
       (
           SELECT string_agg('lex' || g || 'x' || s, ' ')
           FROM generate_series(1, 96) s
       )
FROM generate_series(1, 256) g;
EOF
}

assert_cache_locator_rollback_output() {
    local test_name=$1
    local output=$2
    local baseline=$3
    local session_status=$4
    local old_filenode new_filenode restored_filenode
    local old_blocks new_blocks in_tx_hits
    local in_tx_bytes bytes_before_read bytes_after_read
    local committed_hits rollback_rows

    info "${test_name} output: ${output}"

    if [ "${session_status}" -ne 0 ]; then
        error "${test_name} failed while reading the restored relfilenode \
(status ${session_status}). Full output:\n${output}"
    fi

    old_filenode=$(echo "${output}" | sed -n \
        's/^.*locator_old_filenode=\([0-9]*\).*$/\1/p' | head -1)
    new_filenode=$(echo "${output}" | sed -n \
        's/^.*locator_new_filenode=\([0-9]*\).*$/\1/p' | head -1)
    restored_filenode=$(echo "${output}" | sed -n \
        's/^.*locator_restored_filenode=\([0-9]*\).*$/\1/p' | head -1)
    old_blocks=$(echo "${output}" | sed -n \
        's/^.*locator_old_blocks=\([0-9]*\).*$/\1/p' | head -1)
    new_blocks=$(echo "${output}" | sed -n \
        's/^.*locator_new_blocks=\([0-9]*\).*$/\1/p' | head -1)
    in_tx_hits=$(echo "${output}" | sed -n \
        's/^.*locator_in_tx_hits=\([0-9]*\).*$/\1/p' | head -1)
    in_tx_bytes=$(echo "${output}" | sed -n \
        's/^.*locator_in_tx_bytes=\([0-9]*\).*$/\1/p' | head -1)
    bytes_before_read=$(echo "${output}" | sed -n \
        's/^.*locator_bytes_before_read=\([0-9]*\).*$/\1/p' | head -1)
    committed_hits=$(echo "${output}" | sed -n \
        's/^.*locator_committed_hits=\([0-9]*\).*$/\1/p' | head -1)
    rollback_rows=$(echo "${output}" | sed -n \
        's/^.*locator_rollback_rows=\([0-9]*\).*$/\1/p' | head -1)
    bytes_after_read=$(echo "${output}" | sed -n \
        's/^.*locator_bytes_after_read=\([0-9]*\).*$/\1/p' | head -1)

    if ! [[ "${old_filenode}" =~ ^[0-9]+$ ]] \
        || ! [[ "${new_filenode}" =~ ^[0-9]+$ ]] \
        || ! [[ "${restored_filenode}" =~ ^[0-9]+$ ]] \
        || [ "${new_filenode}" = "${old_filenode}" ] \
        || [ "${restored_filenode}" != "${old_filenode}" ]; then
        error "${test_name} did not replace and restore the index file \
('${old_filenode:-missing}' -> '${new_filenode:-missing}' -> \
'${restored_filenode:-missing}'). Full output:\n${output}"
    fi
    if ! [[ "${old_blocks}" =~ ^[0-9]+$ ]] \
        || ! [[ "${new_blocks}" =~ ^[0-9]+$ ]] \
        || [ "${new_blocks}" -le "${old_blocks}" ]; then
        error "${test_name} did not extend the discarded relfilenode past \
the restored file (${old_blocks:-missing} -> ${new_blocks:-missing} blocks). \
Full output:\n${output}"
    fi
    if [ "${in_tx_hits}" != "276" ]; then
        error "${test_name} did not build the replacement-file cache through \
the normal BM25 path (hits=${in_tx_hits:-missing}). Full output:\n${output}"
    fi
    if ! [[ "${in_tx_bytes}" =~ ^[0-9]+$ ]] \
        || ! [[ "${bytes_before_read}" =~ ^[0-9]+$ ]] \
        || [ "${in_tx_bytes}" -le "${baseline}" ] \
        || [ "${bytes_before_read}" -ne "${in_tx_bytes}" ]; then
        error "${test_name} did not retain the charged replacement-file \
cache through rollback (${baseline} -> ${in_tx_bytes:-missing} -> \
${bytes_before_read:-missing}). Full output:\n${output}"
    fi
    if [ "${committed_hits}" != "20" ] || [ "${rollback_rows}" != "0" ]; then
        error "${test_name} returned the wrong restored rows \
(committed=${committed_hits:-missing}, rolled_back=${rollback_rows:-missing}). \
Full output:\n${output}"
    fi
    if [ "${bytes_after_read}" != "${baseline}" ]; then
        error "${test_name} did not drain discarded-file cache accounting \
(${bytes_before_read:-missing} -> ${bytes_after_read:-missing}, \
baseline ${baseline}). Full output:\n${output}"
    fi
    if ! echo "${output}" | grep -qF "locator_complete"; then
        error "${test_name} did not complete the restored-file read. \
Full output:\n${output}"
    fi
}

# A top-level abort restores the old index relfilenode after a normal query has
# populated the shared cache from the replacement file.
run_cache_locator_top_level_rollback_test() {
    local output session_status baseline

    log "Test: cache locator survives top-level REINDEX rollback"
    prepare_cache_locator_rollback_fixture
    baseline=$(run_sql_value "SELECT bm25_cache_global_estimated_bytes();")

    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
SET pg_textsearch.memtable_cache_enabled = on;
SELECT 'locator_old_filenode=' ||
       pg_relation_filenode('cache_locator_idx');
SELECT 'locator_old_blocks=' ||
       pg_relation_size('cache_locator_idx') /
       current_setting('block_size')::bigint;
BEGIN;
REINDEX INDEX cache_locator_idx;
$(cache_locator_rollback_insert_sql)
SELECT 'locator_new_filenode=' ||
       pg_relation_filenode('cache_locator_idx');
SELECT 'locator_new_blocks=' ||
       pg_relation_size('cache_locator_idx') /
       current_setting('block_size')::bigint;
SELECT 'locator_in_tx_hits=' || count(*) FROM (
    SELECT id FROM cache_locator_docs
    ORDER BY content <@>
        to_bm25query('locatoranchor', 'cache_locator_idx')
    LIMIT 1000
) q;
SELECT 'locator_in_tx_bytes=' ||
       bm25_cache_global_estimated_bytes();
ROLLBACK;
SELECT 'locator_restored_filenode=' ||
       pg_relation_filenode('cache_locator_idx');
SELECT 'locator_bytes_before_read=' ||
       bm25_cache_global_estimated_bytes();
SELECT 'locator_committed_hits=' || count(*) FROM (
    SELECT id FROM cache_locator_docs
    ORDER BY content <@>
        to_bm25query('locatoranchor', 'cache_locator_idx')
    LIMIT 1000
) q;
SELECT 'locator_rollback_rows=' || count(*)
FROM cache_locator_docs
WHERE content LIKE '%rollbackonly%';
SELECT 'locator_bytes_after_read=' ||
       bm25_cache_global_estimated_bytes();
SELECT 'locator_complete';
EOF
)
    session_status=$?
    set -e

    assert_cache_locator_rollback_output \
        "Top-level REINDEX rollback" "${output}" "${baseline}" \
        "${session_status}"
    run_sql_quiet "DROP TABLE cache_locator_docs CASCADE;"
    log "✅ top-level rollback rebuilt the cache from the restored file"
}

# Rolling back to a savepoint switches the relation back to the old file while
# the outer transaction and backend-local wrapper remain active.
run_cache_locator_savepoint_rollback_test() {
    local output session_status baseline

    log "Test: cache locator survives savepoint REINDEX rollback"
    prepare_cache_locator_rollback_fixture
    baseline=$(run_sql_value "SELECT bm25_cache_global_estimated_bytes();")

    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
SET pg_textsearch.memtable_cache_enabled = on;
SELECT 'locator_old_filenode=' ||
       pg_relation_filenode('cache_locator_idx');
SELECT 'locator_old_blocks=' ||
       pg_relation_size('cache_locator_idx') /
       current_setting('block_size')::bigint;
BEGIN;
SAVEPOINT before_reindex;
REINDEX INDEX cache_locator_idx;
$(cache_locator_rollback_insert_sql)
SELECT 'locator_new_filenode=' ||
       pg_relation_filenode('cache_locator_idx');
SELECT 'locator_new_blocks=' ||
       pg_relation_size('cache_locator_idx') /
       current_setting('block_size')::bigint;
SELECT 'locator_in_tx_hits=' || count(*) FROM (
    SELECT id FROM cache_locator_docs
    ORDER BY content <@>
        to_bm25query('locatoranchor', 'cache_locator_idx')
    LIMIT 1000
) q;
SELECT 'locator_in_tx_bytes=' ||
       bm25_cache_global_estimated_bytes();
ROLLBACK TO SAVEPOINT before_reindex;
SELECT 'locator_restored_filenode=' ||
       pg_relation_filenode('cache_locator_idx');
SELECT 'locator_bytes_before_read=' ||
       bm25_cache_global_estimated_bytes();
SELECT 'locator_committed_hits=' || count(*) FROM (
    SELECT id FROM cache_locator_docs
    ORDER BY content <@>
        to_bm25query('locatoranchor', 'cache_locator_idx')
    LIMIT 1000
) q;
SELECT 'locator_rollback_rows=' || count(*)
FROM cache_locator_docs
WHERE content LIKE '%rollbackonly%';
SELECT 'locator_bytes_after_read=' ||
       bm25_cache_global_estimated_bytes();
SELECT 'locator_complete';
COMMIT;
EOF
)
    session_status=$?
    set -e

    assert_cache_locator_rollback_output \
        "Savepoint REINDEX rollback" "${output}" "${baseline}" \
        "${session_status}"
    run_sql_quiet "DROP TABLE cache_locator_docs CASCADE;"
    log "✅ savepoint rollback rebuilt the cache from the restored file"
}

# Prepared rollback selects the old relfilenode in a different backend after
# the preparing backend has populated shared cache state from the new file.
run_cache_locator_prepared_rollback_test() {
    local prepare_output read_output output session_status baseline
    local gid prepared_count

    log "Test: cache locator survives ROLLBACK PREPARED after REINDEX"
    prepare_cache_locator_rollback_fixture
    baseline=$(run_sql_value "SELECT bm25_cache_global_estimated_bytes();")
    gid="cache_locator_reindex_${TEST_PORT}_$$"

    prepare_output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" --set ON_ERROR_STOP=1 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
SET pg_textsearch.memtable_cache_enabled = on;
SELECT 'locator_old_filenode=' ||
       pg_relation_filenode('cache_locator_idx');
SELECT 'locator_old_blocks=' ||
       pg_relation_size('cache_locator_idx') /
       current_setting('block_size')::bigint;
BEGIN;
REINDEX INDEX cache_locator_idx;
$(cache_locator_rollback_insert_sql)
SELECT 'locator_new_filenode=' ||
       pg_relation_filenode('cache_locator_idx');
SELECT 'locator_new_blocks=' ||
       pg_relation_size('cache_locator_idx') /
       current_setting('block_size')::bigint;
SELECT 'locator_in_tx_hits=' || count(*) FROM (
    SELECT id FROM cache_locator_docs
    ORDER BY content <@>
        to_bm25query('locatoranchor', 'cache_locator_idx')
    LIMIT 1000
) q;
SELECT 'locator_in_tx_bytes=' ||
       bm25_cache_global_estimated_bytes();
PREPARE TRANSACTION '${gid}';
EOF
)

    prepared_count=$(run_sql_value "
        SELECT count(*) FROM pg_prepared_xacts WHERE gid = '${gid}';
    ")
    if [ "${prepared_count}" != "1" ]; then
        error "REINDEX cache-locator transaction was not prepared. \
Full output:\n${prepare_output}"
    fi

    run_sql_quiet "ROLLBACK PREPARED '${gid}';"

    set +e
    read_output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" \
        -d "${TEST_DB}" --set ON_ERROR_STOP=1 2>&1 << 'EOF'
\set QUIET on
\pset footer off
\pset tuples_only on
SET pg_textsearch.memtable_cache_enabled = on;
SELECT 'locator_restored_filenode=' ||
       pg_relation_filenode('cache_locator_idx');
SELECT 'locator_bytes_before_read=' ||
       bm25_cache_global_estimated_bytes();
SELECT 'locator_committed_hits=' || count(*) FROM (
    SELECT id FROM cache_locator_docs
    ORDER BY content <@>
        to_bm25query('locatoranchor', 'cache_locator_idx')
    LIMIT 1000
) q;
SELECT 'locator_rollback_rows=' || count(*)
FROM cache_locator_docs
WHERE content LIKE '%rollbackonly%';
SELECT 'locator_bytes_after_read=' ||
       bm25_cache_global_estimated_bytes();
SELECT 'locator_complete';
EOF
)
    session_status=$?
    set -e
    output="${prepare_output}"$'\n'"${read_output}"

    assert_cache_locator_rollback_output \
        "ROLLBACK PREPARED after REINDEX" "${output}" "${baseline}" \
        "${session_status}"
    run_sql_quiet "DROP TABLE cache_locator_docs CASCADE;"
    log "✅ prepared rollback rebuilt the cache from the restored file"
}

# DROP removes the runtime registry entry immediately.  If an initial CREATE
# is dropped in a savepoint and that DROP is rolled back, first access must
# reconstruct the CREATE ownership from PostgreSQL's restored relation state.
# A top-level abort must then remove the reconstructed registry/cache state and
# restore the exact cluster-global accounting baseline.
run_initial_create_drop_rollback_abort_test() {
    local output session_status
    local baseline cache_observation bytes_after index_count

    log "Test: rolled-back DROP preserves initial-CREATE abort ownership"

    run_sql_quiet "
        DROP TABLE IF EXISTS create_drop_abort_docs CASCADE;
        CREATE TABLE create_drop_abort_docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        );
        INSERT INTO create_drop_abort_docs (content)
        SELECT 'create drop abort seed ' || g
        FROM generate_series(1, 25) g;
    "
    baseline=$(run_sql_value "SELECT bm25_cache_global_estimated_bytes();")

    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
BEGIN;
CREATE INDEX create_drop_abort_idx ON create_drop_abort_docs
USING bm25 (content) WITH (text_config='english');
SAVEPOINT drop_index;
DROP INDEX create_drop_abort_idx;
ROLLBACK TO drop_index;
INSERT INTO create_drop_abort_docs (content)
VALUES ('create drop abort charged cache tail');
SELECT 'create_drop_abort_cache=' || result || '|' || estimated_bytes
FROM bm25_cache_cold_build('create_drop_abort_idx');
ROLLBACK;
SELECT 'create_drop_abort_bytes=' ||
       bm25_cache_global_estimated_bytes();
SELECT 'create_drop_abort_index_count=' || count(*)
FROM pg_class
WHERE relname = 'create_drop_abort_idx';
EOF
)
    session_status=$?
    set -e

    info "Rolled-back DROP top-level abort output: ${output}"

    if [ "${session_status}" -ne 0 ]; then
        error "Rolled-back DROP top-level abort session failed \
(status ${session_status}). Full output:\n${output}"
    fi

    cache_observation=$(echo "${output}" | sed -n \
        's/^.*create_drop_abort_cache=\([^[:space:]]*\).*$/\1/p' | head -1)
    bytes_after=$(echo "${output}" | sed -n \
        's/^.*create_drop_abort_bytes=\([0-9]*\).*$/\1/p' | head -1)
    index_count=$(echo "${output}" | sed -n \
        's/^.*create_drop_abort_index_count=\([0-9]*\).*$/\1/p' | head -1)

    if ! [[ "${cache_observation}" =~ ^OK\|[1-9][0-9]*$ ]]; then
        error "Rolled-back DROP setup did not charge the reconstructed \
initial-CREATE cache (got '${cache_observation:-missing}'). \
Full output:\n${output}"
    fi
    if [ "${bytes_after}" != "${baseline}" ]; then
        error "Top-level abort after rolled-back DROP leaked cache \
accounting (${baseline} -> ${bytes_after:-missing}). \
Full output:\n${output}"
    fi
    if [ "${index_count}" != "0" ]; then
        error "Top-level abort after rolled-back DROP retained the \
initially-created index (count=${index_count:-missing})."
    fi

    run_sql_quiet "DROP TABLE create_drop_abort_docs;"
    log "✅ rolled-back DROP retained initial-CREATE abort ownership"
}

# A successful REINDEX replaces the relfilenode with an empty memtable chain.
# The stable shared state's old page count must not make the first post-REINDEX
# page look over threshold and spill prematurely.
run_warm_reindex_chain_count_test() {
    local chain_pages_before chain_pages_after hits

    log "Test: warm successful REINDEX reseeds chain-page accounting"

    run_sql_quiet "
        DROP TABLE IF EXISTS warm_chain_docs CASCADE;
        CREATE TABLE warm_chain_docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        );
        CREATE INDEX warm_chain_docs_bm25 ON warm_chain_docs
        USING bm25 (content) WITH (text_config='english');
        SET pg_textsearch.memtable_pages_threshold = 0;
        INSERT INTO warm_chain_docs (content)
        SELECT 'warm chain before reindex ' || g || ' ' ||
               repeat('filler ', 4)
        FROM generate_series(1, 200) g;
    "

    chain_pages_before=$(run_sql_value "
        SELECT count(*) FROM bm25_memtable_chain('warm_chain_docs_bm25');
    ")
    if ! [[ "${chain_pages_before}" =~ ^[0-9]+$ ]] \
        || [ "${chain_pages_before}" -le 1 ]; then
        error "Warm REINDEX setup did not create a multi-page chain \
(pages=${chain_pages_before:-missing})."
    fi

    run_sql_quiet "REINDEX INDEX warm_chain_docs_bm25;"
    run_sql_quiet "
        SET pg_textsearch.memtable_pages_threshold = 2;
        INSERT INTO warm_chain_docs (content)
        VALUES ('warm chain first page after reindex');
    "

    chain_pages_after=$(run_sql_value "
        SELECT count(*) FROM bm25_memtable_chain('warm_chain_docs_bm25');
    ")
    hits=$(run_sql_value "
        SELECT count(*) FROM (
            SELECT 1 FROM warm_chain_docs
            ORDER BY content <@>
                to_bm25query('warm', 'warm_chain_docs_bm25')
        ) q;
    ")

    if [ "${chain_pages_after}" != "1" ]; then
        error "Successful warm REINDEX reused the old chain-page count; \
the first new page was spilled (before=${chain_pages_before}, \
after=${chain_pages_after:-missing})."
    fi
    if [ "${hits}" != "201" ]; then
        error "Warm REINDEX chain-count test returned ${hits:-missing} \
rows instead of 201."
    fi

    run_sql_quiet "DROP TABLE warm_chain_docs CASCADE;"
    log "✅ warm successful REINDEX used the current relfilenode count"
}

# If a threshold consumer runs after REINDEX but before transaction abort, it
# may reseed against the replacement relfilenode.  Rollback must make that
# cached count stale again so the restored multi-page chain is recounted.
run_warm_reindex_reseed_before_abort_test() {
    local output session_status
    local chain_pages_before chain_pages_during chain_pages_after hits

    log "Test: REINDEX abort invalidates an in-transaction reseed"

    run_sql_quiet "
        DROP TABLE IF EXISTS warm_abort_chain_docs CASCADE;
        CREATE TABLE warm_abort_chain_docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        );
        CREATE INDEX warm_abort_chain_docs_bm25 ON warm_abort_chain_docs
        USING bm25 (content) WITH (text_config='english');
        SET pg_textsearch.memtable_pages_threshold = 0;
        INSERT INTO warm_abort_chain_docs (content)
        SELECT 'warm abort chain before reindex ' || g || ' ' ||
               repeat('filler ', 4)
        FROM generate_series(1, 200) g;
    "

    chain_pages_before=$(run_sql_value "
        SELECT count(*)
        FROM bm25_memtable_chain('warm_abort_chain_docs_bm25');
    ")
    if ! [[ "${chain_pages_before}" =~ ^[0-9]+$ ]] \
        || [ "${chain_pages_before}" -le 1 ]; then
        error "Warm REINDEX abort setup did not create a multi-page chain \
(pages=${chain_pages_before:-missing})."
    fi

    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
SET pg_textsearch.memtable_pages_threshold = ${chain_pages_before};
BEGIN;
REINDEX INDEX warm_abort_chain_docs_bm25;
INSERT INTO warm_abort_chain_docs (content)
VALUES ('warm abort replacement relfilenode reseed');
SELECT 'warm_abort_chain_during=' || count(*)
FROM bm25_memtable_chain('warm_abort_chain_docs_bm25');
ROLLBACK;
INSERT INTO warm_abort_chain_docs (content)
VALUES ('warm abort restored relfilenode threshold');
SELECT 'warm_abort_chain_after=' || count(*)
FROM bm25_memtable_chain('warm_abort_chain_docs_bm25');
SELECT 'warm_abort_hits=' || count(*) FROM (
    SELECT 1 FROM warm_abort_chain_docs
    ORDER BY content <@>
        to_bm25query('abort', 'warm_abort_chain_docs_bm25')
) q;
EOF
)
    session_status=$?
    set -e

    info "Warm REINDEX reseed-before-abort output: ${output}"

    if [ "${session_status}" -ne 0 ]; then
        error "Warm REINDEX reseed-before-abort session failed \
(status ${session_status}). Full output:\n${output}"
    fi

    chain_pages_during=$(echo "${output}" | sed -n \
        's/^.*warm_abort_chain_during=\([0-9]*\).*$/\1/p' | head -1)
    chain_pages_after=$(echo "${output}" | sed -n \
        's/^.*warm_abort_chain_after=\([0-9]*\).*$/\1/p' | head -1)
    hits=$(echo "${output}" | sed -n \
        's/^.*warm_abort_hits=\([0-9]*\).*$/\1/p' | head -1)

    if [ "${chain_pages_during}" != "1" ]; then
        error "REINDEX replacement chain was not reseeded before abort \
(pages=${chain_pages_during:-missing}). Full output:\n${output}"
    fi
    if [ "${chain_pages_after}" != "0" ]; then
        error "REINDEX abort left the replacement relfilenode count \
attached to the restored ${chain_pages_before}-page chain \
(pages=${chain_pages_after:-missing}). Full output:\n${output}"
    fi
    if [ "${hits}" != "201" ]; then
        error "Warm REINDEX reseed-before-abort test returned \
${hits:-missing} rows instead of 201."
    fi

    run_sql_quiet "DROP TABLE warm_abort_chain_docs CASCADE;"
    log "✅ REINDEX abort invalidated the replacement relfilenode count"
}

# A restart clears the shared registry while leaving the index relation on
# disk. The first post-restart REINDEX must therefore attach cold state
# without claiming initial-CREATE ownership, and transaction abort must leave
# that state usable in the same backend.  Its page-count heuristic starts at
# zero, so rollback must force a lazy recount of the restored nonempty chain
# before the next threshold decision.
run_cold_registry_reindex_rollback_test() {
    local output session_status hits chain_pages_before chain_pages_after

    log "Test: cold REINDEX abort reseeds restored chain-page accounting"

    run_sql_quiet "
        DROP TABLE IF EXISTS cold_docs CASCADE;
        CREATE TABLE cold_docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        );
        INSERT INTO cold_docs (content)
        SELECT 'cold registry rollback ' || g
        FROM generate_series(1, 225) g;
        CREATE INDEX cold_docs_bm25 ON cold_docs
        USING bm25 (content) WITH (text_config='english');
        SET pg_textsearch.memtable_pages_threshold = 0;
        INSERT INTO cold_docs (content)
        SELECT 'cold registry rollback tail ' || g || ' ' ||
               repeat('filler ', 4)
        FROM generate_series(1, 200) g;
    "

    chain_pages_before=$(run_sql_value "
        SELECT count(*) FROM bm25_memtable_chain('cold_docs_bm25');
    ")
    if ! [[ "${chain_pages_before}" =~ ^[0-9]+$ ]] \
        || [ "${chain_pages_before}" -le 1 ]; then
        error "Cold REINDEX abort setup did not create a multi-page chain \
(pages=${chain_pages_before:-missing})."
    fi

    pg_ctl restart -D "${DATA_DIR}" -m fast -l "${LOGFILE}" -w \
        >/dev/null 2>&1 || error "Failed to restart PostgreSQL"

    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
SET pg_textsearch.memtable_pages_threshold = 2;
BEGIN;
REINDEX INDEX cold_docs_bm25;
SELECT 'cold_reindex_built_before_abort';
\set ON_ERROR_STOP 0
SELECT 1 / 0;
\set ON_ERROR_STOP 1
ROLLBACK;
INSERT INTO cold_docs (content)
VALUES ('cold registry rollback tail after abort');
SELECT 'cold_reindex_chain_pages=' || count(*)
FROM bm25_memtable_chain('cold_docs_bm25');
SELECT 'cold_reindex_hits=' || count(*) FROM (
    SELECT 1 FROM cold_docs
    ORDER BY content <@> to_bm25query('rollback', 'cold_docs_bm25')
) q;
EOF
)
    session_status=$?
    set -e

    info "Cold-registry REINDEX session output: ${output}"

    if [ "${session_status}" -ne 0 ]; then
        error "Cold-registry REINDEX session failed after rollback \
(status ${session_status}). Full output:\n${output}"
    fi
    if ! echo "${output}" | grep -qF "cold_reindex_built_before_abort" \
        || ! echo "${output}" | grep -qF "division by zero"; then
        error "Cold-registry REINDEX rollback was not fully exercised. \
Full output:\n${output}"
    fi

    hits=$(echo "${output}" | sed -n \
        's/^.*cold_reindex_hits=\([0-9]*\).*$/\1/p' | head -1)
    chain_pages_after=$(echo "${output}" | sed -n \
        's/^.*cold_reindex_chain_pages=\([0-9]*\).*$/\1/p' | head -1)
    if [ "${chain_pages_after}" != "0" ]; then
        error "Cold REINDEX abort left a zero heuristic attached to the \
restored ${chain_pages_before}-page chain; the next threshold insert \
did not spill (pages=${chain_pages_after:-missing}). \
Full output:\n${output}"
    fi
    if [ "${hits}" != "426" ]; then
        error "Cold-registry REINDEX rollback left the index unusable \
(hits=${hits:-missing}). Full output:\n${output}"
    fi

    run_sql_quiet "DROP TABLE cold_docs CASCADE;"
    log "✅ cold REINDEX abort recounted and spilled the restored chain"
}

# REINDEX replaces only the current backend's wrapper.  Transaction-local
# bulk-load accounting accumulated before a savepoint must survive that
# replacement and the savepoint rollback so PRE_COMMIT still spills.
run_reindex_bulk_counter_preservation_test() {
    local output session_status
    local chain_pages_before_commit chain_pages_after_commit hits

    log "Test: REINDEX wrapper replacement preserves bulk-load counters"

    run_sql_quiet "
        DROP TABLE IF EXISTS bulk_counter_docs CASCADE;
        CREATE TABLE bulk_counter_docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        );
        CREATE INDEX bulk_counter_docs_bm25 ON bulk_counter_docs
        USING bm25 (content) WITH (text_config='english');
    "

    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 1;
BEGIN;
INSERT INTO bulk_counter_docs (content)
VALUES ('bulk counter survives savepoint reindex rollback');
SAVEPOINT reindex_savepoint;
REINDEX INDEX bulk_counter_docs_bm25;
SELECT 'bulk_counter_reindex_complete';
ROLLBACK TO reindex_savepoint;
SELECT 'bulk_counter_chain_before_commit=' || count(*)
FROM bm25_memtable_chain('bulk_counter_docs_bm25');
COMMIT;
SELECT 'bulk_counter_chain_after_commit=' || count(*)
FROM bm25_memtable_chain('bulk_counter_docs_bm25');
SELECT 'bulk_counter_hits=' || count(*) FROM (
    SELECT 1 FROM bulk_counter_docs
    ORDER BY content <@>
        to_bm25query('counter', 'bulk_counter_docs_bm25')
) q;
EOF
)
    session_status=$?
    set -e

    info "REINDEX bulk-counter session output: ${output}"

    if [ "${session_status}" -ne 0 ] \
        || ! echo "${output}" | grep -qF "bulk_counter_reindex_complete"; then
        error "REINDEX bulk-counter session did not complete the savepoint \
rollback scenario. Full output:\n${output}"
    fi

    chain_pages_before_commit=$(echo "${output}" | sed -n \
        's/^.*bulk_counter_chain_before_commit=\([0-9]*\).*$/\1/p' \
        | head -1)
    chain_pages_after_commit=$(echo "${output}" | sed -n \
        's/^.*bulk_counter_chain_after_commit=\([0-9]*\).*$/\1/p' \
        | head -1)
    hits=$(echo "${output}" | sed -n \
        's/^.*bulk_counter_hits=\([0-9]*\).*$/\1/p' | head -1)

    if ! [[ "${chain_pages_before_commit}" =~ ^[1-9][0-9]*$ ]]; then
        error "REINDEX bulk-counter setup did not restore a nonempty chain \
before PRE_COMMIT (pages=${chain_pages_before_commit:-missing})."
    fi
    if [ "${chain_pages_after_commit}" != "0" ]; then
        error "REINDEX wrapper replacement lost terms_added_this_xact; \
bulk_load_threshold=1 did not spill at PRE_COMMIT \
(${chain_pages_before_commit} -> ${chain_pages_after_commit:-missing})."
    fi
    if [ "${hits}" != "1" ]; then
        error "REINDEX bulk-counter test returned ${hits:-missing} rows \
instead of 1."
    fi

    run_sql_quiet "DROP TABLE bulk_counter_docs CASCADE;"
    log "✅ REINDEX wrapper preserved PRE_COMMIT bulk-load accounting"
}

run_prepare_bulk_state_case() {
    local case_name="$1"
    local outcome="$2"
    local table_name="$3"
    local index_name="$4"
    local prepared_term="$5"
    local controller_term="$6"
    local gid="pgts_prepare_bulk_${case_name}_${TEST_PORT}_$$"
    local ready_file="${DATA_DIR}/prepare_bulk_${case_name}.ready"
    local release_file="${DATA_DIR}/prepare_bulk_${case_name}.release"
    local output_file="${DATA_DIR}/prepare_bulk_${case_name}.out"
    local failure=
    local chain_after_prepare chain_before_unrelated chain_after_unrelated
    local backend_before backend_after prepared_count
    local prepared_heap_count prepared_index_count
    local controller_heap_count controller_index_count
    local output

    rm -f "${ready_file}" "${release_file}" "${output_file}"

    PGAPPNAME="pgts_prepare_bulk_${case_name}" \
        timeout --signal=TERM --kill-after=2s 45s \
        psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 >"${output_file}" 2>&1 << EOF &
\set QUIET on
SET pg_textsearch.memtable_pages_threshold = 0;
SET pg_textsearch.bulk_load_threshold = 1;
SELECT pg_backend_pid() AS prepare_backend \gset
BEGIN;
INSERT INTO ${table_name} (id, content)
VALUES (1, '${prepared_term} prepared payload');
PREPARE TRANSACTION '${gid}';
\echo prepare_backend_before=:prepare_backend
\! : > '${ready_file}'; while [ ! -f '${release_file}' ]; do sleep 0.05; done
BEGIN;
INSERT INTO prepare_bulk_plain (case_name)
VALUES ('${case_name}');
SELECT pg_backend_pid() AS unrelated_backend \gset
COMMIT;
\echo prepare_backend_after=:unrelated_backend
\echo prepare_unrelated_commit_complete
EOF
    PREPARE_CLIENT_PID=$!

    if ! wait_for_prepare_marker "${ready_file}"; then
        output=$(cat "${output_file}" 2>/dev/null || true)
        stop_prepare_client || true
        prepared_count=$(run_sql_value_bounded "
            SELECT count(*) FROM pg_prepared_xacts
            WHERE gid = '${gid}';
        " || true)
        if [ "${prepared_count}" = "1" ]; then
            run_sql_quiet_bounded "ROLLBACK PREPARED '${gid}';" || true
        fi
        error "PREPARE bulk-state ${case_name} client did not reach the \
post-PREPARE barrier. Full output:\n${output}"
    fi

    chain_after_prepare=$(run_sql_value_bounded "
        SELECT count(*) FROM bm25_memtable_chain('${index_name}');
    ")
    if [ "${chain_after_prepare}" != "0" ]; then
        failure="${failure} PREPARE left ${chain_after_prepare} chain pages;"
    fi

    run_sql_quiet_bounded "
        SET pg_textsearch.bulk_load_threshold = 0;
        SET pg_textsearch.memtable_pages_threshold = 0;
        INSERT INTO ${table_name} (id, content)
        VALUES (2, '${controller_term} controller payload');
    "
    chain_before_unrelated=$(run_sql_value_bounded "
        SELECT count(*) FROM bm25_memtable_chain('${index_name}');
    ")
    if ! [[ "${chain_before_unrelated}" =~ ^[1-9][0-9]*$ ]]; then
        failure="${failure} controller insert did not create a chain;"
    fi

    : >"${release_file}"
    if ! wait_for_prepare_client_exit; then
        failure="${failure} persistent client did not exit within 10 seconds;"
        stop_prepare_client || true
    fi

    output=$(cat "${output_file}" 2>/dev/null || true)
    backend_before=$(echo "${output}" | sed -n \
        's/^prepare_backend_before=\([0-9]*\)$/\1/p' | head -1)
    backend_after=$(echo "${output}" | sed -n \
        's/^prepare_backend_after=\([0-9]*\)$/\1/p' | head -1)
    if ! [[ "${backend_before}" =~ ^[0-9]+$ ]] ||
        [ "${backend_before}" != "${backend_after}" ]; then
        failure="${failure} PREPARE and unrelated transaction used different backends;"
    fi
    if ! echo "${output}" | grep -qF "prepare_unrelated_commit_complete"; then
        failure="${failure} unrelated transaction did not commit;"
    fi

    chain_after_unrelated=$(run_sql_value_bounded "
        SELECT count(*) FROM bm25_memtable_chain('${index_name}');
    ")
    if [ "${chain_after_unrelated}" != "${chain_before_unrelated}" ]; then
        failure="${failure} unrelated commit changed chain pages \
${chain_before_unrelated}->${chain_after_unrelated};"
    fi

    prepared_count=$(run_sql_value_bounded "
        SELECT count(*) FROM pg_prepared_xacts
        WHERE gid = '${gid}';
    ")
    if [ "${prepared_count}" = "1" ]; then
        if [ "${outcome}" = "commit" ]; then
            run_sql_quiet_bounded "COMMIT PREPARED '${gid}';"
        else
            run_sql_quiet_bounded "ROLLBACK PREPARED '${gid}';"
        fi
    else
        failure="${failure} prepared transaction count was ${prepared_count};"
    fi

    prepared_heap_count=$(run_sql_value_bounded "
        SELECT count(*) FROM ${table_name}
        WHERE content LIKE '%${prepared_term}%';
    ")
    prepared_index_count=$(run_sql_value_bounded "
        SELECT count(*)
        FROM (
            SELECT id
            FROM ${table_name}
            ORDER BY content <@>
                to_bm25query('${prepared_term}', '${index_name}')
            LIMIT 10
        ) hits
        JOIN ${table_name} docs USING (id)
        WHERE docs.content LIKE '%${prepared_term}%';
    ")
    controller_heap_count=$(run_sql_value_bounded "
        SELECT count(*) FROM ${table_name}
        WHERE content LIKE '%${controller_term}%';
    ")
    controller_index_count=$(run_sql_value_bounded "
        SELECT count(*)
        FROM (
            SELECT id
            FROM ${table_name}
            ORDER BY content <@>
                to_bm25query('${controller_term}', '${index_name}')
            LIMIT 10
        ) hits
        JOIN ${table_name} docs USING (id)
        WHERE docs.content LIKE '%${controller_term}%';
    ")

    if [ "${outcome}" = "commit" ]; then
        if [ "${prepared_heap_count}" != "1" ] ||
            [ "${prepared_index_count}" != "1" ]; then
            failure="${failure} COMMIT PREPARED lost the prepared row \
(heap=${prepared_heap_count}, index=${prepared_index_count});"
        fi
    elif [ "${prepared_heap_count}" != "0" ] ||
        [ "${prepared_index_count}" != "0" ]; then
        failure="${failure} ROLLBACK PREPARED retained the prepared row \
(heap=${prepared_heap_count}, index=${prepared_index_count});"
    fi

    if [ "${controller_heap_count}" != "1" ] ||
        [ "${controller_index_count}" != "1" ]; then
        failure="${failure} controller row was not preserved \
(heap=${controller_heap_count}, index=${controller_index_count});"
    fi

    info "PREPARE bulk-state ${case_name} output: ${output}"
    if [ -n "${failure}" ]; then
        warn "PREPARE bulk-state ${case_name} failures:${failure}"
        return 1
    fi

    log "✅ PREPARE ${outcome} isolated terminal bulk state"
}

# PREPARE must spill the preparing transaction while errors can still abort
# preparation, then clear backend-local counters before that backend starts a
# new transaction.  A controller insert after the PREPARE barrier leaves a
# fresh chain; the unrelated transaction must not spill that other backend's
# work.  Both two-phase outcomes must retain normal heap/index visibility.
run_prepare_bulk_state_reset_test() {
    local failed=0

    log "Test: PREPARE spills and resets backend-local bulk state"

    run_sql_quiet "
        DROP TABLE IF EXISTS prepare_bulk_commit_docs CASCADE;
        DROP TABLE IF EXISTS prepare_bulk_rollback_docs CASCADE;
        DROP TABLE IF EXISTS prepare_bulk_plain;
        CREATE TABLE prepare_bulk_commit_docs (
            id      bigint PRIMARY KEY,
            content text NOT NULL
        );
        CREATE INDEX prepare_bulk_commit_docs_bm25
            ON prepare_bulk_commit_docs USING bm25 (content)
            WITH (text_config='english');
        CREATE TABLE prepare_bulk_rollback_docs (
            id      bigint PRIMARY KEY,
            content text NOT NULL
        );
        CREATE INDEX prepare_bulk_rollback_docs_bm25
            ON prepare_bulk_rollback_docs USING bm25 (content)
            WITH (text_config='english');
        CREATE TABLE prepare_bulk_plain (case_name text);
    "

    if ! run_prepare_bulk_state_case \
        commit \
        commit \
        prepare_bulk_commit_docs \
        prepare_bulk_commit_docs_bm25 \
        preparedcommitonly \
        controllercommitonly; then
        failed=1
    fi

    if ! run_prepare_bulk_state_case \
        rollback \
        rollback \
        prepare_bulk_rollback_docs \
        prepare_bulk_rollback_docs_bm25 \
        preparedrollbackonly \
        controllerrollbackonly; then
        failed=1
    fi

    run_sql_quiet "
        DROP TABLE prepare_bulk_commit_docs CASCADE;
        DROP TABLE prepare_bulk_rollback_docs CASCADE;
        DROP TABLE prepare_bulk_plain;
    "

    if [ "${failed}" -ne 0 ]; then
        error "PREPARE bulk-state isolation regression failed"
    fi
}

# Initial CREATE ownership lives only in the creating backend, so it cannot be
# serialized into a prepared transaction. A normally committed CREATE must
# clear that ownership so a later transaction in the same backend can prepare.
run_prepare_initial_create_test() {
    local reject_gid="pg_textsearch_create_reject_${TEST_PORT}_$$"
    local later_gid="pg_textsearch_after_create_${TEST_PORT}_$$"
    local output session_status prepared_count
    local baseline cache_observation bytes_after

    log "Test: PREPARE rejects reconstructed initial-CREATE ownership"

    run_sql_quiet "
        DROP TABLE IF EXISTS prepare_docs CASCADE;
        DROP TABLE IF EXISTS prepare_plain;
        CREATE TABLE prepare_docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        );
        INSERT INTO prepare_docs (content)
        SELECT 'prepare create ownership ' || g
        FROM generate_series(1, 25) g;
        CREATE TABLE prepare_plain (id int);
    "
    baseline=$(run_sql_value "SELECT bm25_cache_global_estimated_bytes();")

    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
BEGIN;
CREATE INDEX prepare_docs_bm25 ON prepare_docs
USING bm25 (content) WITH (text_config='english');
SAVEPOINT drop_index;
DROP INDEX prepare_docs_bm25;
ROLLBACK TO drop_index;
INSERT INTO prepare_docs (content)
VALUES ('prepare reconstructed ownership charged cache tail');
SELECT 'prepare_drop_rollback_cache=' || result || '|' || estimated_bytes
FROM bm25_cache_cold_build('prepare_docs_bm25');
PREPARE TRANSACTION '${reject_gid}';
EOF
)
    session_status=$?
    set -e

    info "Initial-CREATE PREPARE output: ${output}"

    cache_observation=$(echo "${output}" | sed -n \
        's/^.*prepare_drop_rollback_cache=\([^[:space:]]*\).*$/\1/p' \
        | head -1)
    if ! [[ "${cache_observation}" =~ ^OK\|[1-9][0-9]*$ ]]; then
        error "PREPARE setup did not charge the reconstructed \
initial-CREATE cache (got '${cache_observation:-missing}'). \
Full output:\n${output}"
    fi

    if [ "${session_status}" -eq 0 ]; then
        run_sql_quiet "ROLLBACK PREPARED '${reject_gid}';"
        error "PREPARE TRANSACTION accepted initial-CREATE ownership \
after DROP INDEX rollback; the reproduction was cleaned with \
ROLLBACK PREPARED. Full output:\n${output}"
    fi
    if ! echo "${output}" | grep -qF \
        "cannot prepare a transaction that created a pg_textsearch index"; then
        error "PREPARE TRANSACTION failed for an unexpected reason. \
Full output:\n${output}"
    fi

    prepared_count=$(run_sql_value "
        SELECT count(*) FROM pg_prepared_xacts
        WHERE gid = '${reject_gid}';
    ")
    if [ "${prepared_count}" != "0" ]; then
        run_sql_quiet "ROLLBACK PREPARED '${reject_gid}';"
        error "Rejected initial CREATE still left a prepared transaction."
    fi
    bytes_after=$(run_sql_value "SELECT bm25_cache_global_estimated_bytes();")
    if [ "${bytes_after}" != "${baseline}" ]; then
        error "Rejected PREPARE after rolled-back DROP leaked cache \
accounting (${baseline} -> ${bytes_after})."
    fi
    if [ "$(run_sql_value "
        SELECT count(*) FROM pg_class
        WHERE relname = 'prepare_docs_bm25';
    ")" != "0" ]; then
        error "Rejected initial CREATE did not roll back its index."
    fi

    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
CREATE INDEX prepare_docs_bm25 ON prepare_docs
USING bm25 (content) WITH (text_config='english');
SELECT 'committed_create_complete';
BEGIN;
INSERT INTO prepare_plain VALUES (1);
PREPARE TRANSACTION '${later_gid}';
SELECT 'later_prepare_succeeded';
EOF
)
    session_status=$?
    set -e

    info "Post-CREATE PREPARE output: ${output}"

    if [ "${session_status}" -ne 0 ] \
        || ! echo "${output}" | grep -qF "committed_create_complete" \
        || ! echo "${output}" | grep -qF "later_prepare_succeeded"; then
        error "Normal CREATE commit left ownership that affected a later \
transaction. Full output:\n${output}"
    fi

    prepared_count=$(run_sql_value "
        SELECT count(*) FROM pg_prepared_xacts
        WHERE gid = '${later_gid}';
    ")
    if [ "${prepared_count}" != "1" ]; then
        error "Later transaction was not prepared successfully."
    fi

    run_sql_quiet "ROLLBACK PREPARED '${later_gid}';"
    run_sql_quiet "
        DROP TABLE prepare_docs CASCADE;
        DROP TABLE prepare_plain;
    "
    log "✅ PREPARE ownership guard and commit clearing passed"
}

# Verify both REINDEX abort boundaries. A build-time failure must leave the
# old cache untouched. If REINDEX finishes and the surrounding transaction
# later aborts, the cache may remain discarded, but the stable shared state
# must still serve the restored on-disk index without leaking accounting.
run_failed_reindex_test() {
    local output session_status
    local generation_before generation_after_failure
    local generation_after_rollback
    local cache_bytes_before cache_bytes_after_failure
    local cache_bytes_after_rollback cache_bytes_after_rebuild
    local hits_after_failure hits_after_rollback

    log "Test: failed and rolled-back REINDEX preserve stable state"

    run_sql_quiet "
        DROP TABLE IF EXISTS docs CASCADE;
        DROP FUNCTION IF EXISTS maybe_fail_reindex(text);

        CREATE FUNCTION maybe_fail_reindex(value text)
        RETURNS text
        LANGUAGE plpgsql
        IMMUTABLE
        PARALLEL UNSAFE
        AS \$\$
        BEGIN
            IF current_setting(
                    'tapir_test.reindex_fail', true
                ) = 'on'
            THEN
                RAISE EXCEPTION 'forced reindex failure';
            END IF;
            RETURN value;
        END
        \$\$;

        CREATE TABLE docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        );
        INSERT INTO docs (content)
        SELECT 'row ' || g || ' failure rollback'
        FROM generate_series(1, 200) g;
        CREATE INDEX docs_bm25 ON docs
        USING bm25 (maybe_fail_reindex(content))
        WITH (text_config='english');
    "

    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
INSERT INTO docs (content)
SELECT 'tail ' || g || ' failure rollback cached before failed reindex'
FROM generate_series(201, 225) g;
SELECT 'failed_reindex_cache_bytes_before=' || estimated_bytes
FROM bm25_cache_cold_build('docs_bm25');
SELECT 'failed_reindex_generation_before=' ||
       bm25_cache_bump_spill_generation('docs_bm25');
SELECT 'failed_reindex_summary_before=' ||
       replace(bm25_summarize_index('docs_bm25'), E'\n', ' ');
\! if psql -X -h "${SOCKET_DIR}" -p ${TEST_PORT} -d "${TEST_DB}" -v ON_ERROR_STOP=1 -q -c "SET tapir_test.reindex_fail = 'on'; REINDEX INDEX docs_bm25;" 2>&1; then echo B-reindex-unexpected-success; else echo B-reindex-failed-as-expected; fi
SELECT 'failed_reindex_summary_after=' ||
       replace(bm25_summarize_index('docs_bm25'), E'\n', ' ');
SELECT 'failed_reindex_generation_after_failure=' ||
       bm25_cache_bump_spill_generation('docs_bm25');
SELECT 'failed_reindex_cache_bytes_after_failure=' ||
       bm25_cache_global_estimated_bytes();
SELECT 'failed_reindex_hits_after_failure=' || count(*) FROM (
    SELECT 1 FROM docs
    ORDER BY maybe_fail_reindex(content) <@>
        to_bm25query('rollback', 'docs_bm25')
) q;
\! if psql -X -h "${SOCKET_DIR}" -p ${TEST_PORT} -d "${TEST_DB}" -v ON_ERROR_STOP=1 -q -c "BEGIN; REINDEX INDEX docs_bm25; SELECT 'B-reindex-built-before-abort'; SELECT 1 / 0;" 2>&1; then echo B-postbuild-abort-unexpected-success; else echo B-postbuild-abort-observed; fi
SELECT 'postbuild_abort_summary=' ||
       replace(bm25_summarize_index('docs_bm25'), E'\n', ' ');
SELECT 'failed_reindex_generation_after_rollback=' ||
       bm25_cache_bump_spill_generation('docs_bm25');
SELECT 'failed_reindex_cache_bytes_after_rollback=' ||
       bm25_cache_global_estimated_bytes();
SELECT 'failed_reindex_hits_after_rollback=' || count(*) FROM (
    SELECT 1 FROM docs
    ORDER BY maybe_fail_reindex(content) <@>
        to_bm25query('rollback', 'docs_bm25')
) q;
SELECT 'failed_reindex_cache_bytes_after_rebuild=' ||
       bm25_cache_global_estimated_bytes();
EOF
)
    session_status=$?
    set -e

    info "Failed-REINDEX session output: ${output}"

    if [ "${session_status}" -ne 0 ]; then
        error "Backend A failed after the expected REINDEX error \
(status ${session_status}). Full output:\n${output}"
    fi
    if ! echo "${output}" | grep -qF "forced reindex failure"; then
        error "Nested REINDEX did not report the injected failure. \
Full output:\n${output}"
    fi
    if ! echo "${output}" | grep -qF "B-reindex-failed-as-expected"; then
        error "Nested REINDEX failure status was not observed. \
Full output:\n${output}"
    fi
    if echo "${output}" | grep -qF "B-reindex-unexpected-success"; then
        error "Nested REINDEX unexpectedly succeeded. Full output:\n${output}"
    fi
    if ! echo "${output}" | grep -qF "B-reindex-built-before-abort" \
        || ! echo "${output}" | grep -qF "division by zero" \
        || ! echo "${output}" | grep -qF "B-postbuild-abort-observed"; then
        error "Post-build transaction abort was not exercised fully. \
Full output:\n${output}"
    fi
    if echo "${output}" | grep -qF "B-postbuild-abort-unexpected-success"; then
        error "Post-build transaction unexpectedly committed. \
Full output:\n${output}"
    fi
    local marker
    for marker in \
        "failed_reindex_summary_before=" \
        "failed_reindex_summary_after=" \
        "postbuild_abort_summary="; do
        if ! echo "${output}" | grep -qF "${marker}"; then
            error "Failed REINDEX test missed required marker '${marker}'. \
Full output:\n${output}"
        fi
    done

    generation_before=$(echo "${output}" | sed -n \
        's/^.*failed_reindex_generation_before=\([0-9]*\).*$/\1/p' \
        | head -1)
    generation_after_failure=$(echo "${output}" | sed -n \
        's/^.*failed_reindex_generation_after_failure=\([0-9]*\).*$/\1/p' \
        | head -1)
    generation_after_rollback=$(echo "${output}" | sed -n \
        's/^.*failed_reindex_generation_after_rollback=\([0-9]*\).*$/\1/p' \
        | head -1)
    if ! [[ "${generation_before}" =~ ^[0-9]+$ ]] \
        || ! [[ "${generation_after_failure}" =~ ^[0-9]+$ ]] \
        || ! [[ "${generation_after_rollback}" =~ ^[0-9]+$ ]]; then
        error "Could not parse failed-REINDEX generations \
('${generation_before}' -> '${generation_after_failure}' -> \
'${generation_after_rollback}')."
    fi
    if [ "${generation_after_failure}" -ne $((generation_before + 1)) ]; then
        error "Failed REINDEX changed or replaced the pre-existing \
shared state generation (${generation_before} -> \
${generation_after_failure})."
    fi
    if [ "${generation_after_rollback}" -ne \
        $((generation_after_failure + 2)) ]; then
        error "Aborting after REINDEX finalization replaced or discarded \
the pre-existing shared state (${generation_after_failure} -> \
${generation_after_rollback})."
    fi

    cache_bytes_before=$(echo "${output}" | sed -n \
        's/^.*failed_reindex_cache_bytes_before=\([0-9]*\).*$/\1/p' \
        | head -1)
    cache_bytes_after_failure=$(echo "${output}" | sed -n \
        's/^.*failed_reindex_cache_bytes_after_failure=\([0-9]*\).*$/\1/p' \
        | head -1)
    cache_bytes_after_rollback=$(echo "${output}" | sed -n \
        's/^.*failed_reindex_cache_bytes_after_rollback=\([0-9]*\).*$/\1/p' \
        | head -1)
    cache_bytes_after_rebuild=$(echo "${output}" | sed -n \
        's/^.*failed_reindex_cache_bytes_after_rebuild=\([0-9]*\).*$/\1/p' \
        | head -1)
    if ! [[ "${cache_bytes_before}" =~ ^[0-9]+$ ]] \
        || ! [[ "${cache_bytes_after_failure}" =~ ^[0-9]+$ ]] \
        || ! [[ "${cache_bytes_after_rollback}" =~ ^[0-9]+$ ]] \
        || ! [[ "${cache_bytes_after_rebuild}" =~ ^[0-9]+$ ]]; then
        error "Could not parse failed-REINDEX cache accounting \
('${cache_bytes_before}', '${cache_bytes_after_failure}', \
'${cache_bytes_after_rollback}', '${cache_bytes_after_rebuild}')."
    fi
    if [ "${cache_bytes_before}" -le 0 ]; then
        error "Failed REINDEX setup did not populate the runtime cache."
    fi
    if [ "${cache_bytes_after_failure}" -ne "${cache_bytes_before}" ]; then
        error "Build-time REINDEX failure changed the existing cache \
accounting (${cache_bytes_before} -> ${cache_bytes_after_failure})."
    fi
    if [ "${cache_bytes_after_rollback}" -ne 0 ] \
        || [ "${cache_bytes_after_rebuild}" -ne "${cache_bytes_before}" ]; then
        error "Post-finalization REINDEX abort leaked or failed to rebuild \
cache accounting (${cache_bytes_before} -> \
${cache_bytes_after_rollback} -> ${cache_bytes_after_rebuild})."
    fi

    hits_after_failure=$(echo "${output}" | sed -n \
        's/^.*failed_reindex_hits_after_failure=\([0-9]*\).*$/\1/p' \
        | head -1)
    hits_after_rollback=$(echo "${output}" | sed -n \
        's/^.*failed_reindex_hits_after_rollback=\([0-9]*\).*$/\1/p' \
        | head -1)
    if [ "${hits_after_failure}" != "225" ] \
        || [ "${hits_after_rollback}" != "225" ]; then
        error "Failed or rolled-back REINDEX left the old index unusable \
(after failure=${hits_after_failure:-missing}, \
after rollback=${hits_after_rollback:-missing})."
    fi

    run_sql_quiet "
        DROP TABLE docs CASCADE;
        DROP FUNCTION maybe_fail_reindex(text);
    "
    log "✅ failed and rolled-back REINDEX preserved stable state"
}

# prepare_docs_table — fresh, intentionally-bloated docs table.
#
# 30k rows + fillfactor=10 → ~4000-6000 heap pages. DELETE of the
# tail leaves ~1000 live rows but the heap retains its page count
# until rewrite (autovacuum is disabled at table level). This makes
# the OLD heap span many blocks while the NEW heap (post-rewrite) is
# small — so any stale CTID retained by a backend with a primed
# cache will point well beyond the new heap's EOF, surfacing the
# "could not read blocks" bug deterministically on v1.2.0.
prepare_docs_table() {
    run_sql_quiet "
        DROP TABLE IF EXISTS docs CASCADE;
        CREATE TABLE docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        ) WITH (autovacuum_enabled=false, fillfactor=10);

        INSERT INTO docs (content)
        SELECT 'row ' || g || ' lorem ipsum dolor sit amet '
               || md5(g::text)
        FROM generate_series(1, 30000) g;

        CREATE INDEX docs_bm25 ON docs USING bm25 (content)
            WITH (text_config='english');
    "
    run_sql_quiet "DELETE FROM docs WHERE id > 1000;"
    run_sql_quiet "ANALYZE docs;"
}

prepare_parallel_docs_table() {
    run_sql_quiet "
        DROP TABLE IF EXISTS docs CASCADE;
        CREATE TABLE docs (
            id      bigserial PRIMARY KEY,
            content text NOT NULL
        ) WITH (autovacuum_enabled=false);

        INSERT INTO docs (content)
        SELECT 'parallel row ' || g || ' lorem ipsum dolor sit amet '
               || repeat(md5(g::text), 2)
        FROM generate_series(1, 100000) g;

        ALTER TABLE docs SET (parallel_workers = 2);
        ANALYZE docs;
        SET max_parallel_maintenance_workers = 2;
        SET max_parallel_workers = 4;
        SET maintenance_work_mem = '256MB';
        SET min_parallel_table_scan_size = 0;
        CREATE INDEX docs_bm25 ON docs USING bm25 (content)
            WITH (text_config='english');
    "
}

# run_rewrite_test test_name rewrite_sql expect_heap_shrink rewrite_count
#                  require_parallel
#
# Drive both backends from one psql session: Backend A primes its
# TpLocalIndexState cache via INSERT, samples pre-rewrite heap size,
# then spawns Backend B (psql via `\!`) to perform the supplied
# rewrite, then continues using its (potentially stale) cached state
# to INSERT 50 'moonlight' rows and force a spill.
#
# After the heredoc completes, all SETUP INVARIANTS are checked
# BEFORE any bug-class assertion (so a degraded test environment
# can't masquerade as a bug-fix regression):
#   - no storage-corruption errors in session output
#   - all session markers were emitted
#   - bm25_spill_index returned a real segment root
#   - pre_heap_pages >= 1000 (bloat sufficient)
#   - heap actually shrank (skipped when expect_heap_shrink=0)
#
# Then the BUG-CLASS ASSERTIONS run:
#   - docs_persisted == live heap row count
#   - top-100 'lorem' index scan returns exactly 100 (no errors)
#   - exactly 50 live 'moonlight' rows are returned through the index
#
# Args:
#   $1: human-readable test name
#   $2: SQL Backend B should run (the heap/index rewrite)
#   $3: 1 if the rewrite is expected to compact the heap, 0 otherwise
#       (REINDEX INDEX only touches the index relfilenode)
#   $4: number of rewrites to run while Backend A retains its cached state
#   $5: 1 if every rewrite must launch a parallel worker, 0 otherwise
run_rewrite_test() {
    local test_name="$1"
    local rewrite_sql="$2"
    local expect_heap_shrink="$3"
    local rewrite_count="$4"
    local require_parallel="$5"
    local rewrite_steps=""
    local rewrite_no

    log "Test: ${test_name} (issue #390)"

    if [ "${require_parallel}" = "1" ]; then
        prepare_parallel_docs_table
    else
        prepare_docs_table
    fi

    for ((rewrite_no = 1; rewrite_no <= rewrite_count; rewrite_no++)); do
        rewrite_steps+="\\! if psql -X -h \"${SOCKET_DIR}\""
        rewrite_steps+=" -p ${TEST_PORT} -d \"${TEST_DB}\""
        rewrite_steps+=" -v ON_ERROR_STOP=1 -q"
        rewrite_steps+=" -c \"${rewrite_sql}\""
        rewrite_steps+="; then echo B-rewrite-ok-${rewrite_no};"
        rewrite_steps+=" else echo B-rewrite-failed-${rewrite_no}; fi"
        rewrite_steps+=$'\n'
        rewrite_steps+="SELECT 'post_rewrite_summary_${rewrite_no}=' ||"
        rewrite_steps+=$'\n'
        rewrite_steps+="       replace(bm25_summarize_index("
        rewrite_steps+="'docs_bm25'), E'\\\\n', ' ');"
        rewrite_steps+=$'\n'
        rewrite_steps+="SELECT 'post_rewrite_generation_${rewrite_no}=' ||"
        rewrite_steps+=$'\n'
        rewrite_steps+="       bm25_cache_bump_spill_generation("
        rewrite_steps+="'docs_bm25');"
        rewrite_steps+=$'\n'
    done

    # Bash heredoc with unquoted EOF so ${SOCKET_DIR}, ${TEST_PORT},
    # ${TEST_DB}, and ${rewrite_steps} all expand at script-render time.
    # Backend A stops on its first SQL error. Each nested Backend B
    # command emits a success marker only if its rewrite exits zero;
    # psql's \! does not propagate nested command status reliably.
    #
    # NOTE on shell quoting: ${rewrite_sql} is interpolated INSIDE
    # double quotes after `\! psql ... -c`. Any SQL string passed
    # here must therefore contain no unescaped double quotes. All
    # four current callers use only single-quoted SQL literals — if
    # a future case needs double quotes, switch the inner -c to a
    # here-string fed to psql instead.
    local output session_status
    set +e
    output=$(psql -X -h "${SOCKET_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" \
        --set ON_ERROR_STOP=1 2>&1 << EOF
\set QUIET on
\pset footer off
\pset tuples_only on
-- Backend A: prime local_state_cache by inserting into the index.
-- The 'unicorn' term is unique to these rows so we can isolate them
-- later. 500 rows extend the heap further, so any CTID captured here
-- will reference a high block number.
INSERT INTO docs (content)
SELECT 'pre ' || g || ' unicorn rainbow sparkle ' || md5(('h'||g)::text)
FROM generate_series(1, 500) g;

-- Sample pre-rewrite heap pages HERE, immediately before the
-- rewrite, so the post-rewrite "heap shrank" invariant has zero
-- slack (matches the exact moment Backend B is about to act on).
SELECT 'pre_heap_pages=' || (pg_relation_size('docs')::bigint / 8192);
SELECT 'pre_rewrite_generation=' ||
       bm25_cache_bump_spill_generation('docs_bm25');
SELECT 'A-primed' AS marker;

-- Backend B: one or more heap/index rewrites. Each runs in a separate
-- psql process via \! while Backend A's session stays open and retains
-- the local-state wrapper cached before the first rewrite.
${rewrite_steps}

-- Backend A is still the same session. Its cached TpLocalIndexState
-- must remain valid because REINDEX resets the shared state in place.
SELECT 'A-post-rewrite' AS marker;

-- Write through the same stable wrapper.
INSERT INTO docs (content)
SELECT 'late ' || g || ' lorem moonlight ' || md5(('l'||g)::text)
FROM generate_series(1, 50) g;

-- Force a spill. On the v1.2.0 bug this is what materialises the
-- corruption: the OLD in-memory memtable (with OLD CTIDs) gets
-- written into the NEW index file as a stale L0 segment.
-- bm25_spill_index returns the new L0 segment's root block number
-- (always > 0 because block 0 is the metapage) or NULL if nothing
-- was spilled.
SELECT 'spill_segment_root=' || bm25_spill_index('docs_bm25');
SELECT 'A-spilled' AS marker;
EOF
)
    session_status=$?
    set -e

    info "Backend A session output: ${output}"

    # ----- SETUP INVARIANT CHECKS (must pass before bug assertions) -----

    # 1. Backend A must itself exit successfully. This catches SQL
    #    errors before marker/count parsing can accidentally mask them.
    if [ "${session_status}" -ne 0 ]; then
        error "Backend A session exited with status ${session_status}. \
Full output:\n${output}"
    fi

    # 2. No corruption or generic SQL errors may appear anywhere,
    #    including output from nested Backend B psql processes.
    if echo "${output}" | grep -qiE "could not read blocks|invalid \
(page|segment)|XX001|(^|[[:space:]])ERROR:"; then
        error "Backend session produced an SQL/storage error. \
Output:\n${output}"
    fi
    if echo "${output}" | grep -qF "B-rewrite-failed-"; then
        error "A nested rewrite command failed. Output:\n${output}"
    fi

    # 3. Session and every nested rewrite must emit their required
    #    markers. In particular, a failed nested psql cannot be hidden
    #    by outer psql continuing after \!.
    local marker
    for marker in "A-primed" "A-post-rewrite" "A-spilled"; do
        if ! echo "${output}" | grep -qF "${marker}"; then
            error "Backend A session never reached marker \
'${marker}'. Full output:\n${output}"
        fi
    done

    for ((rewrite_no = 1; rewrite_no <= rewrite_count; rewrite_no++)); do
        for marker in \
            "B-rewrite-ok-${rewrite_no}" \
            "post_rewrite_summary_${rewrite_no}=" \
            "post_rewrite_generation_${rewrite_no}="; do
            if ! echo "${output}" | grep -qF "${marker}"; then
                error "Rewrite ${rewrite_no} never reached required marker \
'${marker}'. Full output:\n${output}"
            fi
        done
    done

    # 4. A designated parallel case must prove every nested REINDEX
    #    actually launched at least one worker.
    if [ "${require_parallel}" = "1" ]; then
        local parallel_launches
        parallel_launches=$(echo "${output}" |
            grep -Ec "parallel index build: launched [1-9][0-9]* " || true)
        if [ "${parallel_launches}" -ne "${rewrite_count}" ]; then
            error "Expected ${rewrite_count} parallel rewrites, observed \
${parallel_launches}. Full output:\n${output}"
        fi
    fi

    # 5. The spill generation belongs to the stable shared state.
    #    It must advance monotonically across every rewrite; resetting
    #    to 1 proves the shared state was destructively replaced.
    local previous_generation current_generation
    previous_generation=$(echo "${output}" | sed -n \
        's/^.*pre_rewrite_generation=\([0-9]*\).*$/\1/p' | head -1)
    if ! [[ "${previous_generation}" =~ ^[0-9]+$ ]]; then
        error "Failed to parse pre-rewrite spill generation \
(got '${previous_generation}')."
    fi
    for ((rewrite_no = 1; rewrite_no <= rewrite_count; rewrite_no++)); do
        current_generation=$(echo "${output}" | sed -n \
            "s/^.*post_rewrite_generation_${rewrite_no}=\\([0-9]*\\).*$/\\1/p" \
            | head -1)
        if ! [[ "${current_generation}" =~ ^[0-9]+$ ]]; then
            error "Failed to parse spill generation after rewrite \
${rewrite_no} (got '${current_generation}')."
        fi
        if [ "${current_generation}" -le "${previous_generation}" ]; then
            error "Shared-state spill generation did not advance across \
rewrite ${rewrite_no} (${previous_generation} -> ${current_generation}); \
the registry state was replaced instead of reset in place."
        fi
        previous_generation="${current_generation}"
    done

    # 6. Spill must have produced a real segment. bm25_spill_index
    #    returns a BlockNumber (always > 0 for a non-empty spill) or
    #    NULL when no segment was written.
    local spill_segment_root
    spill_segment_root=$(echo "${output}" \
        | sed -n 's/^.*spill_segment_root=\([0-9]*\).*$/\1/p' | head -1)
    if [ -z "${spill_segment_root}" ] \
        || [ "${spill_segment_root}" -le 0 ]; then
        error "bm25_spill_index returned no segment root \
(got '${spill_segment_root:-empty}'); the late inserts should have \
populated the memtable before the explicit spill."
    fi
    info "Spill segment root block: ${spill_segment_root}"

    # 7. Pre-rewrite heap page count (parsed from session output).
    local pre_heap_pages
    pre_heap_pages=$(echo "${output}" \
        | sed -n 's/^.*pre_heap_pages=\([0-9]*\).*$/\1/p' | head -1)
    if ! [[ "${pre_heap_pages}" =~ ^[0-9]+$ ]]; then
        error "Failed to obtain pre-rewrite heap page count from \
session output (got '${pre_heap_pages}'). Test setup broken."
    fi
    info "Pre-rewrite heap pages: ${pre_heap_pages}"

    # 8. Bloat threshold. Promoted from warn to error: the entire
    #    test thesis depends on the OLD heap dwarfing the NEW one.
    #    Without enough bloat, stale CTIDs may still land on valid
    #    blocks and the bug becomes silently invisible.
    if [ "${pre_heap_pages}" -lt 1000 ]; then
        error "Pre-rewrite heap is too small (${pre_heap_pages} \
pages, need >= 1000). Test setup is degenerate — bloat is insufficient \
for stale-CTID detection."
    fi

    # 9. Heap-rewrite invariant: confirms the rewrite actually
    #    happened. Skipped for REINDEX INDEX (touches only the index
    #    relfilenode, not the heap).
    if [ "${expect_heap_shrink}" = "1" ]; then
        local heap_size_now
        heap_size_now=$(run_sql_value \
            "SELECT pg_relation_size('docs')/8192;")
        if ! [[ "${heap_size_now}" =~ ^[0-9]+$ ]]; then
            error "Failed to obtain post-rewrite heap size \
(got '${heap_size_now}')."
        fi
        info "Post-rewrite heap pages: ${heap_size_now}"
        if [ "${heap_size_now}" -ge "${pre_heap_pages}" ]; then
            error "Heap was not compacted by '${test_name}' \
(pre=${pre_heap_pages}, post=${heap_size_now}). Test setup invalid — \
either bloat was insufficient or the rewrite did not happen."
        fi
    fi

    # ----- BUG-CLASS ASSERTIONS (only after setup is verified) -----

    # A1. docs_persisted must equal live heap row count. The bug
    #     inflates the segment doc count above the actual row count
    #     because the stale memtable was spilled as a second L0
    #     segment whose entries reference long-dead CTIDs.
    local heap_rows seg_docs
    heap_rows=$(run_sql_value "SELECT count(*) FROM docs;")
    seg_docs=$(run_sql_value "
        SELECT (regexp_match(
            bm25_summarize_index('docs_bm25'),
            'docs_persisted: ([0-9]+)'
        ))[1]::bigint;
    ")
    info "Heap live rows: ${heap_rows}, docs_persisted: ${seg_docs}"
    if [ -z "${seg_docs}" ]; then
        error "Failed to parse docs_persisted from \
bm25_summarize_index output:\n$(run_sql \
"SELECT bm25_summarize_index('docs_bm25');")"
    fi
    if [ "${seg_docs}" != "${heap_rows}" ]; then
        error "BUG (issue #390): docs_persisted (${seg_docs}) does \
not match live heap row count (${heap_rows}). Excess docs indicate \
a stale segment was written from a pre-rewrite memtable.\n\nFull \
summary:\n$(run_sql "SELECT bm25_summarize_index('docs_bm25');")"
    fi

    # A2. Top-100 'lorem' scan. The bug manifests as "could not read
    #     blocks" when the stale segment's top-k results point past
    #     the new heap's EOF. Wrap in `set +e/-e` so psql's non-zero
    #     exit on the corruption error does NOT abort before we
    #     inspect the captured stderr.
    local scan_q
    set +e
    scan_q=$(run_sql_value "
        SELECT count(*)::text FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('lorem', 'docs_bm25')
            LIMIT 100
        ) s;
    ")
    set -e
    if echo "${scan_q}" | grep -qiE "could not read blocks|invalid \
(page|segment)|error"; then
        error "BUG (issue #390): top-100 'lorem' index scan failed: \
${scan_q}"
    fi
    if [ "${scan_q}" != "100" ]; then
        error "BUG (issue #390): top-100 index scan for 'lorem' \
returned '${scan_q}' rows, expected 100."
    fi

    # A3. Post-rewrite late-insert visibility: 'moonlight' appears
    #     only in the 50 late rows Backend A inserted via the
    #     (potentially stale) cache. md5-hex output uses only [0-9a-f]
    #     so 'moonlight' (m,n,i,g,h,t) cannot accidentally appear in
    #     any pre-rewrite row's md5 suffix — the inner-join LIKE
    #     filter is therefore provably tight. Single query handles
    #     both the error-detection and count-verification roles
    #     (no double-scan).
    local moonlight_count
    set +e
    moonlight_count=$(run_sql_value "
        SELECT count(*)::text FROM (
            SELECT id FROM docs
            ORDER BY content <@> to_bm25query('moonlight', 'docs_bm25')
            LIMIT 100
        ) s
        JOIN docs d ON d.id = s.id
        WHERE d.content LIKE '%moonlight%';
    ")
    set -e
    if echo "${moonlight_count}" | grep -qiE "could not read blocks\
|invalid (page|segment)|error"; then
        error "BUG (issue #390): post-rewrite 'moonlight' query \
failed: ${moonlight_count}"
    fi
    if [ "${moonlight_count}" != "50" ]; then
        error "BUG (issue #390): expected 50 live 'moonlight' rows \
via BM25 index, got '${moonlight_count}'. The post-rewrite late \
inserts were not correctly indexed in the new file."
    fi

    run_sql_quiet "DROP TABLE docs CASCADE;"
    log "✅ ${test_name}: no stale CTIDs after rewrite"
}

main() {
    log "Starting pg_textsearch multi-backend reindex test (#390)..."

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"

    setup_test_db

    run_cache_locator_top_level_rollback_test
    run_cache_locator_savepoint_rollback_test
    run_cache_locator_prepared_rollback_test
    run_initial_create_drop_rollback_abort_test
    run_warm_reindex_chain_count_test
    run_warm_reindex_reseed_before_abort_test
    run_cold_registry_reindex_rollback_test
    run_reindex_bulk_counter_preservation_test
    run_prepare_bulk_state_reset_test
    run_prepare_initial_create_test
    run_failed_reindex_test

    # Cover the heap/index rewrite paths from issue #390 as a class,
    # then exercise repeated serial and actual parallel REINDEX while
    # Backend A retains the same TpLocalIndexState wrapper.
    run_rewrite_test \
        "ALTER TABLE ADD COLUMN ... GENERATED STORED" \
        "ALTER TABLE docs ADD COLUMN content_tsv tsvector GENERATED \
ALWAYS AS (to_tsvector('english', content)) STORED;" \
        1 \
        1 \
        0

    run_rewrite_test \
        "VACUUM FULL docs" \
        "VACUUM FULL docs;" \
        1 \
        1 \
        0

    run_rewrite_test \
        "CLUSTER docs USING docs_pkey" \
        "CLUSTER docs USING docs_pkey;" \
        1 \
        1 \
        0

    run_rewrite_test \
        "repeated serial REINDEX INDEX docs_bm25" \
        "SET max_parallel_maintenance_workers = 0; \
REINDEX INDEX docs_bm25;" \
        0 \
        3 \
        0

    run_rewrite_test \
        "parallel REINDEX INDEX docs_bm25" \
        "SET max_parallel_maintenance_workers = 2; \
SET max_parallel_workers = 4; \
SET maintenance_work_mem = '256MB'; \
SET min_parallel_table_scan_size = 0; \
REINDEX INDEX docs_bm25;" \
        0 \
        1 \
        1

    log "🎉 multi_backend_reindex.sh: all tests passed"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
