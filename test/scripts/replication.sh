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

# shellcheck source=replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

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
    primary_count=$(search_count "${PRIMARY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
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
    standby_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
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
    standby_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Standby search for 'algorithms' after inserts: \
${standby_count} results"

    # Should find more than before (original 2 + 3 new = 5)
    if [ "$standby_count" -lt 4 ]; then
        error "Expected at least 4 results, got ${standby_count}"
    fi

    # Verify counts match primary
    local primary_count
    primary_count=$(search_count "${PRIMARY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")

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
    standby_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Standby search after spill: ${standby_count} results"

    local primary_count
    primary_count=$(search_count "${PRIMARY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")

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
    pre_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
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
    promoted_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
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
    post_insert_count=$(search_count "${STANDBY_PORT}" docs content \
        "algorithms" "docs_bm25_idx")
    log "Search after insert on promoted standby: \
${post_insert_count}"

    if [ "$post_insert_count" -le "$promoted_count" ]; then
        error "Insert on promoted standby not reflected in search \
(before: ${promoted_count}, after: ${post_insert_count})"
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
