#!/bin/bash
#
# Logical replication test for pg_textsearch
#
# Tests:
#   1. Initial table sync copies data; index built on subscriber works
#   2. Ongoing INSERTs replicate and become searchable
#   3. DELETEs replicate and are removed from search results
#   4. UPDATEs replicate and search reflects new content
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PUB_PORT=55438
SUB_PORT=55439
TEST_DB=logical_repl_test
PUB_DIR="${SCRIPT_DIR}/../tmp_logical_pub"
SUB_DIR="${SCRIPT_DIR}/../tmp_logical_sub"
PUB_LOG="${PUB_DIR}/postgres.log"
SUB_LOG="${SUB_DIR}/postgres.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() {
    echo -e "${GREEN}[$(date '+%H:%M:%S')] $1${NC}"
}

warn() {
    echo -e "${YELLOW}[$(date '+%H:%M:%S')] WARNING: $1${NC}"
}

error() {
    echo -e "${RED}[$(date '+%H:%M:%S')] ERROR: $1${NC}"
    exit 1
}

cleanup() {
    local exit_code=$?
    log "Cleaning up test environment..."
    if [ $exit_code -ne 0 ]; then
        warn "Test failed - dumping logs:"
        if [ -f "${PUB_DIR}/log/postgres.log" ]; then
            echo "=== PUBLISHER LOG (last 80 lines) ==="
            tail -80 "${PUB_DIR}/log/postgres.log" 2>/dev/null || true
        fi
        if [ -f "${SUB_DIR}/log/postgres.log" ]; then
            echo "=== SUBSCRIBER LOG (last 80 lines) ==="
            tail -80 "${SUB_DIR}/log/postgres.log" 2>/dev/null || true
        fi
    fi
    for dir in "${SUB_DIR}" "${PUB_DIR}"; do
        if [ -f "${dir}/postmaster.pid" ]; then
            pg_ctl stop -D "${dir}" -m fast 2>/dev/null ||
                pg_ctl stop -D "${dir}" -m immediate 2>/dev/null \
                || true
        fi
    done
    rm -rf "${PUB_DIR}" "${SUB_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

pub_sql() {
    psql -p "${PUB_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

pub_sql_quiet() {
    psql -p "${PUB_PORT}" -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

sub_sql() {
    psql -p "${SUB_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

sub_sql_quiet() {
    psql -p "${SUB_PORT}" -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

search_count() {
    local port=$1
    local query=$2
    local index=$3
    psql -p "${port}" -d "${TEST_DB}" -tA -c "
        SELECT count(*) FROM (
            SELECT id FROM docs
            WHERE content <@> to_bm25query('${query}', '${index}') < 0
            ORDER BY content <@> to_bm25query('${query}', '${index}')
            LIMIT 100
        ) t;" 2>/dev/null
}

# Wait for subscriber to sync up to publisher's current state
wait_for_sync() {
    local max_wait=${1:-15}
    local i=0
    log "Waiting for logical replication sync (max ${max_wait}s)..."
    while [ $i -lt $max_wait ]; do
        # Check subscription state
        local state
        state=$(sub_sql_quiet \
            "SELECT srsubstate FROM pg_subscription_rel LIMIT 1;")
        local pub_count
        pub_count=$(pub_sql_quiet "SELECT count(*) FROM docs;")
        local sub_count
        sub_count=$(sub_sql_quiet "SELECT count(*) FROM docs;")
        if [ "$pub_count" = "$sub_count" ] && [ -n "$pub_count" ]; then
            log "Subscriber synced: ${sub_count} rows"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    warn "Subscriber may not have synced after ${max_wait}s"
    warn "  Publisher rows: $(pub_sql_quiet 'SELECT count(*) FROM docs;')"
    warn "  Subscriber rows: $(sub_sql_quiet 'SELECT count(*) FROM docs;')"
    return 1
}

setup_publisher() {
    log "Setting up publisher on port ${PUB_PORT}..."
    rm -rf "${PUB_DIR}"
    mkdir -p "${PUB_DIR}"

    initdb -D "${PUB_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1

    cat >> "${PUB_DIR}/postgresql.conf" << EOF
port = ${PUB_PORT}
shared_buffers = 128MB
max_connections = 20
log_min_messages = notice
logging_collector = on
log_filename = 'postgres.log'
shared_preload_libraries = 'pg_textsearch'
wal_level = logical
max_replication_slots = 5
max_wal_senders = 5
EOF

    echo "local replication all trust" >> "${PUB_DIR}/pg_hba.conf"
    echo "host replication all 127.0.0.1/32 trust" >> \
        "${PUB_DIR}/pg_hba.conf"
    echo "host all all 127.0.0.1/32 trust" >> \
        "${PUB_DIR}/pg_hba.conf"

    pg_ctl start -D "${PUB_DIR}" -l "${PUB_LOG}" -w
    createdb -p "${PUB_PORT}" "${TEST_DB}"
    psql -p "${PUB_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

setup_subscriber() {
    log "Setting up subscriber on port ${SUB_PORT}..."
    rm -rf "${SUB_DIR}"
    mkdir -p "${SUB_DIR}"

    initdb -D "${SUB_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1

    cat >> "${SUB_DIR}/postgresql.conf" << EOF
port = ${SUB_PORT}
shared_buffers = 128MB
max_connections = 20
log_min_messages = notice
logging_collector = on
log_filename = 'postgres.log'
shared_preload_libraries = 'pg_textsearch'
EOF

    pg_ctl start -D "${SUB_DIR}" -l "${SUB_LOG}" -w
    createdb -p "${SUB_PORT}" "${TEST_DB}"
    psql -p "${SUB_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

# ---------------------------------------------------------------
# Test 1: Initial sync + index on subscriber
# ---------------------------------------------------------------
test_initial_sync() {
    log "=== Test 1: Initial sync and subscriber index ==="

    # Create table and data on publisher
    log "Creating table and data on publisher..."
    pub_sql "
        CREATE TABLE docs (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL
        );
    " >/dev/null

    pub_sql "
        INSERT INTO docs (content) VALUES
            ('database systems and query optimization'),
            ('full text search algorithms'),
            ('postgresql extension development guide'),
            ('bm25 ranking algorithm implementation'),
            ('distributed systems architecture'),
            ('machine learning algorithms overview'),
            ('natural language processing methods'),
            ('search engine optimization techniques'),
            ('cloud computing infrastructure'),
            ('microservices design patterns');
    " >/dev/null

    # Create publication
    pub_sql "CREATE PUBLICATION docs_pub FOR TABLE docs;" >/dev/null

    # Create matching table on subscriber (no data yet)
    sub_sql "
        CREATE TABLE docs (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL
        );
    " >/dev/null

    # Create subscription (this triggers initial sync)
    sub_sql "
        CREATE SUBSCRIPTION docs_sub
            CONNECTION 'host=127.0.0.1 port=${PUB_PORT}
                dbname=${TEST_DB}'
            PUBLICATION docs_pub;
    "

    wait_for_sync

    # Now create bm25 index on subscriber
    log "Creating bm25 index on subscriber..."
    sub_sql "
        CREATE INDEX docs_bm25_idx ON docs USING bm25(content)
            WITH (text_config='english');
    "

    # Also create bm25 index on publisher for comparison
    pub_sql "
        CREATE INDEX docs_bm25_idx ON docs USING bm25(content)
            WITH (text_config='english');
    " >/dev/null

    # Compare search results
    local pub_count
    pub_count=$(search_count "${PUB_PORT}" "algorithms" "docs_bm25_idx")
    local sub_count
    sub_count=$(search_count "${SUB_PORT}" "algorithms" "docs_bm25_idx")

    log "Publisher search 'algorithms': ${pub_count} results"
    log "Subscriber search 'algorithms': ${sub_count} results"

    if [ -z "$sub_count" ] || [ "$sub_count" -lt 1 ]; then
        error "Subscriber search returned no results!"
    fi

    if [ "$sub_count" != "$pub_count" ]; then
        error "Result count mismatch: pub=${pub_count} sub=${sub_count}"
    fi

    log "Test 1 PASSED: Initial sync and subscriber index work"
}

# ---------------------------------------------------------------
# Test 2: Ongoing INSERTs replicate and become searchable
# ---------------------------------------------------------------
test_insert_replication() {
    log "=== Test 2: INSERT replication ==="

    log "Inserting new documents on publisher..."
    pub_sql "
        INSERT INTO docs (content) VALUES
            ('advanced algorithms for sorting and searching'),
            ('graph algorithms and network analysis'),
            ('parallel algorithms on modern hardware');
    " >/dev/null

    wait_for_sync

    local pub_count
    pub_count=$(search_count "${PUB_PORT}" "algorithms" "docs_bm25_idx")
    local sub_count
    sub_count=$(search_count "${SUB_PORT}" "algorithms" "docs_bm25_idx")

    log "Publisher 'algorithms' after inserts: ${pub_count}"
    log "Subscriber 'algorithms' after inserts: ${sub_count}"

    if [ "$sub_count" != "$pub_count" ]; then
        error "Result mismatch after inserts: pub=${pub_count} \
sub=${sub_count}"
    fi

    log "Test 2 PASSED: INSERT replication works"
}

# ---------------------------------------------------------------
# Test 3: DELETEs replicate and are removed from search
# ---------------------------------------------------------------
test_delete_replication() {
    log "=== Test 3: DELETE replication ==="

    local pre_count
    pre_count=$(search_count "${SUB_PORT}" "algorithms" "docs_bm25_idx")
    log "Subscriber 'algorithms' before delete: ${pre_count}"

    # Delete one of the algorithm documents
    pub_sql "DELETE FROM docs WHERE id = 2;" >/dev/null

    wait_for_sync

    local pub_count
    pub_count=$(search_count "${PUB_PORT}" "algorithms" "docs_bm25_idx")
    local sub_count
    sub_count=$(search_count "${SUB_PORT}" "algorithms" "docs_bm25_idx")

    log "Publisher 'algorithms' after delete: ${pub_count}"
    log "Subscriber 'algorithms' after delete: ${sub_count}"

    if [ "$sub_count" != "$pub_count" ]; then
        error "Result mismatch after delete: pub=${pub_count} \
sub=${sub_count}"
    fi

    # Verify the count decreased
    if [ "$sub_count" -ge "$pre_count" ]; then
        error "DELETE not reflected in search results \
(before: ${pre_count}, after: ${sub_count})"
    fi

    log "Test 3 PASSED: DELETE replication works"
}

# ---------------------------------------------------------------
# Test 4: UPDATEs replicate and search reflects new content
# ---------------------------------------------------------------
test_update_replication() {
    log "=== Test 4: UPDATE replication ==="

    # Update a document to contain 'algorithms'
    log "Updating document on publisher..."
    pub_sql "
        UPDATE docs SET content = 'updated document about algorithms'
        WHERE id = 1;
    " >/dev/null

    wait_for_sync

    local pub_count
    pub_count=$(search_count "${PUB_PORT}" "algorithms" "docs_bm25_idx")
    local sub_count
    sub_count=$(search_count "${SUB_PORT}" "algorithms" "docs_bm25_idx")

    log "Publisher 'algorithms' after update: ${pub_count}"
    log "Subscriber 'algorithms' after update: ${sub_count}"

    if [ "$sub_count" != "$pub_count" ]; then
        error "Result mismatch after update: pub=${pub_count} \
sub=${sub_count}"
    fi

    # Verify the updated doc appears in search
    local updated_content
    updated_content=$(sub_sql_quiet "
        SELECT content FROM docs WHERE id = 1;")
    if [ "$updated_content" != "updated document about algorithms" ]; then
        error "UPDATE not replicated: content='${updated_content}'"
    fi

    log "Test 4 PASSED: UPDATE replication works"
}

main() {
    log "Starting pg_textsearch logical replication test..."

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"
    command -v initdb >/dev/null 2>&1 || error "initdb not found"

    setup_publisher
    setup_subscriber

    test_initial_sync
    test_insert_replication
    test_delete_replication
    test_update_replication

    log "All logical replication tests passed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
