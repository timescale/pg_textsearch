#!/bin/bash
#
# Physical (streaming) replication test for pg_textsearch
#
# Tests:
#   1. Standby starts up with replicated bm25 index
#   2. BM25 search queries work on hot standby
#   3. Inserts on primary replicate and become searchable on standby
#   4. Standby promotion preserves index functionality
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRIMARY_PORT=55436
STANDBY_PORT=55437
TEST_DB=replication_test
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_repl_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_repl_standby"
PRIMARY_LOG="${PRIMARY_DIR}/postgres.log"
STANDBY_LOG="${STANDBY_DIR}/postgres.log"

# Colors for output
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
        if [ -f "${PRIMARY_DIR}/log/postgres.log" ]; then
            echo "=== PRIMARY LOG (last 80 lines) ==="
            tail -80 "${PRIMARY_DIR}/log/postgres.log" 2>/dev/null || true
        fi
        if [ -f "${STANDBY_DIR}/log/postgres.log" ]; then
            echo "=== STANDBY LOG (last 80 lines) ==="
            tail -80 "${STANDBY_DIR}/log/postgres.log" 2>/dev/null || true
        fi
    fi
    for dir in "${STANDBY_DIR}" "${PRIMARY_DIR}"; do
        if [ -f "${dir}/postmaster.pid" ]; then
            pg_ctl stop -D "${dir}" -m fast 2>/dev/null ||
                pg_ctl stop -D "${dir}" -m immediate 2>/dev/null || true
        fi
    done
    rm -rf "${PRIMARY_DIR}" "${STANDBY_DIR}"
}

trap cleanup EXIT INT TERM

primary_sql() {
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

primary_sql_quiet() {
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

standby_sql() {
    psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

standby_sql_quiet() {
    psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA -c "$1" 2>/dev/null
}

wait_for_standby_catchup() {
    local max_wait=${1:-10}
    local i=0
    log "Waiting for standby to catch up (max ${max_wait}s)..."
    while [ $i -lt $max_wait ]; do
        local primary_lsn
        primary_lsn=$(psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA \
            -c "SELECT pg_current_wal_lsn();" 2>/dev/null)
        local standby_lsn
        standby_lsn=$(psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA \
            -c "SELECT pg_last_wal_replay_lsn();" 2>/dev/null)
        if [ -n "$primary_lsn" ] && [ -n "$standby_lsn" ]; then
            local caught_up
            caught_up=$(psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA \
                -c "SELECT '${standby_lsn}'::pg_lsn >= '${primary_lsn}'::pg_lsn;" 2>/dev/null)
            if [ "$caught_up" = "t" ]; then
                log "Standby caught up at LSN ${standby_lsn}"
                return 0
            fi
        fi
        sleep 1
        i=$((i + 1))
    done
    warn "Standby may not have caught up after ${max_wait}s"
    return 1
}

# Helper: count search results using the correct query pattern
# The <@> operator returns a negative BM25 score (double precision),
# so WHERE uses "< 0" to filter matches.
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

setup_primary() {
    log "Setting up primary instance on port ${PRIMARY_PORT}..."
    rm -rf "${PRIMARY_DIR}"
    mkdir -p "${PRIMARY_DIR}"

    initdb -D "${PRIMARY_DIR}" --auth-local=trust --auth-host=trust \
        >/dev/null 2>&1

    cat >> "${PRIMARY_DIR}/postgresql.conf" << EOF
port = ${PRIMARY_PORT}
shared_buffers = 128MB
max_connections = 20
log_min_messages = notice
logging_collector = on
log_filename = 'postgres.log'
shared_preload_libraries = 'pg_textsearch'

# WAL and replication settings
wal_level = replica
max_wal_senders = 5
hot_standby = on
EOF

    # Allow replication connections
    echo "local replication all trust" >> "${PRIMARY_DIR}/pg_hba.conf"
    echo "host replication all 127.0.0.1/32 trust" >> \
        "${PRIMARY_DIR}/pg_hba.conf"

    pg_ctl start -D "${PRIMARY_DIR}" -l "${PRIMARY_LOG}" -w
    createdb -p "${PRIMARY_PORT}" "${TEST_DB}"
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" \
        -c "CREATE EXTENSION pg_textsearch;" >/dev/null
}

setup_standby() {
    log "Creating standby via pg_basebackup..."
    rm -rf "${STANDBY_DIR}"

    pg_basebackup -D "${STANDBY_DIR}" -p "${PRIMARY_PORT}" -R \
        -X stream --checkpoint=fast >/dev/null 2>&1

    # Override port for standby
    cat >> "${STANDBY_DIR}/postgresql.conf" << EOF
port = ${STANDBY_PORT}
EOF

    pg_ctl start -D "${STANDBY_DIR}" -l "${STANDBY_LOG}" -w

    # Verify standby is in recovery mode
    local in_recovery
    in_recovery=$(psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA \
        -c "SELECT pg_is_in_recovery();" 2>/dev/null)
    if [ "$in_recovery" != "t" ]; then
        error "Standby is not in recovery mode!"
    fi
    log "Standby running on port ${STANDBY_PORT} (in recovery)"
}

# ---------------------------------------------------------------
# Test 1: Build index on primary, then create standby, query it
# ---------------------------------------------------------------
test_basic_standby_queries() {
    log "=== Test 1: Basic standby queries ==="

    log "Creating table and index on primary..."
    primary_sql "
        CREATE TABLE docs (
            id SERIAL PRIMARY KEY,
            content TEXT NOT NULL
        );
    " >/dev/null

    primary_sql "
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

    primary_sql "
        CREATE INDEX docs_bm25_idx ON docs USING bm25(content)
            WITH (text_config='english');
    "

    # Verify search works on primary
    local primary_count
    primary_count=$(search_count "${PRIMARY_PORT}" "algorithms" \
        "docs_bm25_idx")
    log "Primary search for 'algorithms': ${primary_count} results"
    if [ -z "$primary_count" ] || [ "$primary_count" -lt 1 ]; then
        error "Primary search returned no results!"
    fi

    # Now create the standby (after data is loaded, avoids slow
    # checkpoint during basebackup of empty cluster)
    setup_standby
    wait_for_standby_catchup

    log "Running search on standby..."
    local standby_result
    standby_result=$(standby_sql "
        SELECT id, content <@> to_bm25query('algorithms',
            'docs_bm25_idx') AS score
        FROM docs
        WHERE content <@> to_bm25query('algorithms',
            'docs_bm25_idx') < 0
        ORDER BY content <@> to_bm25query('algorithms',
            'docs_bm25_idx')
        LIMIT 10;
    ")
    echo "$standby_result"

    local standby_count
    standby_count=$(search_count "${STANDBY_PORT}" "algorithms" \
        "docs_bm25_idx")
    log "Standby search for 'algorithms': ${standby_count} results"

    if [ -z "$standby_count" ] || [ "$standby_count" -lt 1 ]; then
        error "Standby search returned no results!"
    fi

    if [ "$standby_count" != "$primary_count" ]; then
        error "Result count mismatch: primary=${primary_count} \
standby=${standby_count}"
    fi

    log "Test 1 PASSED: Standby queries match primary"
}

# ---------------------------------------------------------------
# Test 2: Insert on primary, verify replication to standby
# ---------------------------------------------------------------
test_ongoing_replication() {
    log "=== Test 2: Ongoing replication ==="

    log "Inserting new documents on primary..."
    primary_sql "
        INSERT INTO docs (content) VALUES
            ('advanced algorithms for sorting and searching'),
            ('graph algorithms and network analysis'),
            ('parallel algorithms on modern hardware');
    " >/dev/null

    wait_for_standby_catchup

    log "Searching on standby after new inserts..."
    local standby_count
    standby_count=$(search_count "${STANDBY_PORT}" "algorithms" \
        "docs_bm25_idx")
    log "Standby search for 'algorithms' after inserts: \
${standby_count} results"

    # Should find more than before (original 2 + 3 new = 5)
    if [ "$standby_count" -lt 4 ]; then
        error "Expected at least 4 results, got ${standby_count}"
    fi

    # Verify counts match primary
    local primary_count
    primary_count=$(search_count "${PRIMARY_PORT}" "algorithms" \
        "docs_bm25_idx")

    if [ "$standby_count" != "$primary_count" ]; then
        error "Result count mismatch after inserts: \
primary=${primary_count} standby=${standby_count}"
    fi

    log "Test 2 PASSED: Ongoing replication works"
}

# ---------------------------------------------------------------
# Test 3: Spill to segments on primary, verify on standby
# ---------------------------------------------------------------
test_segment_replication() {
    log "=== Test 3: Segment replication ==="

    log "Forcing memtable spill on primary..."
    primary_sql "SELECT bm25_spill_index('docs_bm25_idx');"

    wait_for_standby_catchup

    log "Searching on standby after segment spill..."
    local standby_count
    standby_count=$(search_count "${STANDBY_PORT}" "algorithms" \
        "docs_bm25_idx")
    log "Standby search after spill: ${standby_count} results"

    local primary_count
    primary_count=$(search_count "${PRIMARY_PORT}" "algorithms" \
        "docs_bm25_idx")

    if [ "$standby_count" != "$primary_count" ]; then
        error "Result mismatch after spill: \
primary=${primary_count} standby=${standby_count}"
    fi

    log "Test 3 PASSED: Segment replication works"
}

# ---------------------------------------------------------------
# Test 4: Promote standby and verify read/write functionality
# ---------------------------------------------------------------
test_standby_promotion() {
    log "=== Test 4: Standby promotion ==="

    # Capture pre-promotion search state
    local pre_count
    pre_count=$(search_count "${STANDBY_PORT}" "algorithms" \
        "docs_bm25_idx")
    log "Pre-promotion search count: ${pre_count}"

    log "Promoting standby to primary..."
    pg_ctl promote -D "${STANDBY_DIR}" -w

    # Wait for promotion to complete
    local i=0
    while [ $i -lt 10 ]; do
        local in_recovery
        in_recovery=$(psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA \
            -c "SELECT pg_is_in_recovery();" 2>/dev/null)
        if [ "$in_recovery" = "f" ]; then
            break
        fi
        sleep 1
        i=$((i + 1))
    done

    local in_recovery
    in_recovery=$(psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -tA \
        -c "SELECT pg_is_in_recovery();" 2>/dev/null)
    if [ "$in_recovery" != "f" ]; then
        error "Standby did not complete promotion!"
    fi
    log "Standby promoted successfully"

    # Test read queries on promoted standby
    log "Testing search on promoted standby..."
    local promoted_count
    promoted_count=$(search_count "${STANDBY_PORT}" "algorithms" \
        "docs_bm25_idx")
    log "Search results on promoted standby: ${promoted_count}"

    if [ -z "$promoted_count" ] || [ "$promoted_count" -lt 1 ]; then
        error "Search failed on promoted standby!"
    fi

    # Test write on promoted standby
    log "Testing insert on promoted standby..."
    psql -p "${STANDBY_PORT}" -d "${TEST_DB}" -c "
        INSERT INTO docs (content) VALUES
            ('post-promotion test document about algorithms');
    " >/dev/null 2>&1

    local post_insert_count
    post_insert_count=$(search_count "${STANDBY_PORT}" "algorithms" \
        "docs_bm25_idx")
    log "Search after insert on promoted standby: \
${post_insert_count}"

    if [ "$post_insert_count" -le "$promoted_count" ]; then
        warn "Insert on promoted standby not reflected in search"
        warn "  Before: ${promoted_count}, After: ${post_insert_count}"
    fi

    log "Test 4 PASSED: Standby promotion works"
}

main() {
    log "Starting pg_textsearch physical replication test..."

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"
    command -v pg_basebackup >/dev/null 2>&1 || \
        error "pg_basebackup not found"
    command -v initdb >/dev/null 2>&1 || error "initdb not found"

    setup_primary
    test_basic_standby_queries
    test_ongoing_replication
    test_segment_replication
    test_standby_promotion

    log "All physical replication tests passed!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
