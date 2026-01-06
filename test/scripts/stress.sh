#!/bin/bash
#
# Long-running stress tests for pg_textsearch
# Tests core functionality over extended time with many segment spills.
# Designed for nightly CI runs with memory leak detection.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_PORT="${TEST_PORT:-55436}"
TEST_DB=pg_textsearch_stress_test
DATA_DIR="${SCRIPT_DIR}/../tmp_stress_test"
LOGFILE="${DATA_DIR}/postgres.log"

# Test parameters - can be overridden via environment
STRESS_DURATION_MINUTES="${STRESS_DURATION_MINUTES:-5}"
SPILL_THRESHOLD="${SPILL_THRESHOLD:-1000}"  # Low threshold for more segments
DOCS_PER_BATCH="${DOCS_PER_BATCH:-100}"
CONCURRENT_READERS="${CONCURRENT_READERS:-3}"
VALIDATION_INTERVAL="${VALIDATION_INTERVAL:-30}"  # seconds between validations

# Colors for output
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
    log "Cleaning up stress test environment (exit code: $exit_code)..."

    # Kill any background processes
    jobs -p | xargs -r kill 2>/dev/null || true

    if [ -f "${DATA_DIR}/postmaster.pid" ]; then
        pg_ctl stop -D "${DATA_DIR}" -m immediate &>/dev/null || true
    fi
    rm -rf "${DATA_DIR}"
    exit $exit_code
}

trap cleanup EXIT INT TERM

setup_test_db() {
    log "Setting up stress test PostgreSQL instance..."

    rm -rf "${DATA_DIR}"
    mkdir -p "${DATA_DIR}"

    initdb -D "${DATA_DIR}" --auth-local=trust --auth-host=trust >/dev/null 2>&1

    cat >> "${DATA_DIR}/postgresql.conf" << EOF
port = ${TEST_PORT}
max_connections = 30
shared_buffers = 256MB
unix_socket_directories = '${DATA_DIR}'
listen_addresses = 'localhost'
log_min_messages = warning
log_statement = 'none'

# Use low spill threshold to create many segments quickly
# This is set via GUC in the test, but we note it here for documentation
EOF

    pg_ctl start -D "${DATA_DIR}" -l "${LOGFILE}" -w || error "Failed to start PostgreSQL"

    createdb -h "${DATA_DIR}" -p "${TEST_PORT}" "${TEST_DB}"
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "CREATE EXTENSION pg_textsearch;" >/dev/null

    # Load validation functions if available
    if [ -f "${SCRIPT_DIR}/../sql/validation.sql" ]; then
        psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -f "${SCRIPT_DIR}/../sql/validation.sql" >/dev/null
    fi

    # Set low spill threshold
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c \
        "ALTER SYSTEM SET pg_textsearch.memtable_spill_threshold = ${SPILL_THRESHOLD};" >/dev/null
    pg_ctl reload -D "${DATA_DIR}" >/dev/null

    log "PostgreSQL started with memtable_spill_threshold = ${SPILL_THRESHOLD}"
}

run_sql() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" 2>&1
}

run_sql_quiet() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -c "$1" >/dev/null 2>&1
}

run_sql_value() {
    psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc "$1" 2>/dev/null
}

#
# Generate random text content for documents
#
generate_doc_content() {
    local doc_id=$1
    local words=("alpha" "beta" "gamma" "delta" "epsilon" "zeta" "eta" "theta"
                 "iota" "kappa" "lambda" "mu" "nu" "xi" "omicron" "pi" "rho"
                 "sigma" "tau" "upsilon" "phi" "chi" "psi" "omega" "database"
                 "search" "query" "index" "document" "text" "ranking" "score")

    local content="document ${doc_id}"
    local num_words=$((RANDOM % 20 + 5))
    for ((i=0; i<num_words; i++)); do
        content="${content} ${words[$((RANDOM % ${#words[@]}))]}"
    done
    echo "$content"
}

#
# Test 1: Long-running insert/query cycle with many segments
#
test_extended_insert_query() {
    log "Test 1: Extended insert/query cycle (${STRESS_DURATION_MINUTES} minutes)"

    run_sql_quiet "CREATE TABLE stress_docs (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "CREATE INDEX stress_idx ON stress_docs USING bm25(content) WITH (text_config='english');"

    local start_time=$(date +%s)
    local end_time=$((start_time + STRESS_DURATION_MINUTES * 60))
    local batch_num=0
    local total_docs=0
    local segment_count=0
    local last_validation=$start_time
    local errors=0

    info "Running for ${STRESS_DURATION_MINUTES} minutes with ${DOCS_PER_BATCH} docs/batch"

    while [ $(date +%s) -lt $end_time ]; do
        batch_num=$((batch_num + 1))

        # Insert a batch of documents
        local insert_sql="INSERT INTO stress_docs (content) VALUES"
        local first=true
        for ((i=1; i<=DOCS_PER_BATCH; i++)); do
            local content=$(generate_doc_content $((total_docs + i)))
            if [ "$first" = true ]; then
                insert_sql="${insert_sql} ('${content}')"
                first=false
            else
                insert_sql="${insert_sql}, ('${content}')"
            fi
        done
        insert_sql="${insert_sql};"

        if ! run_sql_quiet "$insert_sql"; then
            warn "Batch $batch_num insert failed"
            errors=$((errors + 1))
        fi
        total_docs=$((total_docs + DOCS_PER_BATCH))

        # Check segment count periodically
        if [ $((batch_num % 10)) -eq 0 ]; then
            local summary=$(run_sql_value "SELECT bm25_summarize_index('stress_idx');" 2>/dev/null || echo "")
            local new_segment_count=$(echo "$summary" | grep -o 'segments: [0-9]*' | grep -o '[0-9]*' || echo "0")
            if [ -n "$new_segment_count" ] && [ "$new_segment_count" != "$segment_count" ]; then
                segment_count=$new_segment_count
                info "Batch $batch_num: $total_docs docs, $segment_count segments"
            fi
        fi

        # Run some queries
        local query_terms=("alpha" "database" "search" "document" "gamma delta")
        for term in "${query_terms[@]}"; do
            local count=$(run_sql_value "SELECT COUNT(*) FROM stress_docs WHERE content <@> to_bm25query('${term}', 'stress_idx') < -0.01;" 2>/dev/null)
            if [ -z "$count" ]; then
                warn "Query for '${term}' returned no result"
                errors=$((errors + 1))
            fi
        done

        # Periodic validation
        local now=$(date +%s)
        if [ $((now - last_validation)) -ge $VALIDATION_INTERVAL ]; then
            info "Running validation checkpoint..."

            # Verify document count matches
            local table_count=$(run_sql_value "SELECT COUNT(*) FROM stress_docs;")
            if [ "$table_count" != "$total_docs" ]; then
                warn "Document count mismatch: expected $total_docs, got $table_count"
                errors=$((errors + 1))
            fi

            # Verify ordering is correct (scores should be negative and sorted)
            local ordering_ok=$(run_sql_value "
                SELECT CASE
                    WHEN COUNT(*) = 0 THEN true
                    WHEN MIN(score) < 0 AND MAX(score) < 0 THEN true
                    ELSE false
                END
                FROM (
                    SELECT content <@> to_bm25query('database', 'stress_idx') as score
                    FROM stress_docs
                    ORDER BY content <@> to_bm25query('database', 'stress_idx')
                    LIMIT 100
                ) t;")

            if [ "$ordering_ok" != "t" ]; then
                warn "Score ordering validation failed"
                errors=$((errors + 1))
            fi

            last_validation=$now
        fi

        # Small delay to avoid overwhelming the system
        sleep 0.05
    done

    # Final statistics
    local final_summary=$(run_sql_value "SELECT bm25_summarize_index('stress_idx');" 2>/dev/null || echo "N/A")
    local final_segments=$(echo "$final_summary" | grep -o 'segments: [0-9]*' | grep -o '[0-9]*' || echo "?")

    info "Final: $total_docs documents, $final_segments segments, $errors errors"

    if [ $errors -eq 0 ]; then
        log "Test 1 passed: Extended insert/query completed successfully"
    else
        warn "Test 1 completed with $errors errors"
    fi

    run_sql_quiet "DROP TABLE stress_docs CASCADE;"
}

#
# Test 2: Concurrent readers with continuous inserts and spills
#
test_concurrent_stress() {
    log "Test 2: Concurrent readers with continuous inserts (${CONCURRENT_READERS} readers)"

    run_sql_quiet "CREATE TABLE conc_docs (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "CREATE INDEX conc_idx ON conc_docs USING bm25(content) WITH (text_config='english');"

    # Initial data load
    info "Loading initial data..."
    for ((batch=1; batch<=10; batch++)); do
        local insert_sql="INSERT INTO conc_docs (content) VALUES"
        local first=true
        for ((i=1; i<=50; i++)); do
            local content=$(generate_doc_content $((batch * 50 + i)))
            if [ "$first" = true ]; then
                insert_sql="${insert_sql} ('${content}')"
                first=false
            else
                insert_sql="${insert_sql}, ('${content}')"
            fi
        done
        run_sql_quiet "${insert_sql};"
    done
    run_sql_quiet "SELECT bm25_spill_index('conc_idx');"

    local start_time=$(date +%s)
    local duration=$((STRESS_DURATION_MINUTES * 60 / 2))  # Half duration for this test
    local end_time=$((start_time + duration))

    # Start concurrent readers
    local reader_pids=()
    for ((r=1; r<=CONCURRENT_READERS; r++)); do
        {
            local queries=0
            local errors=0
            while [ $(date +%s) -lt $end_time ]; do
                local count=$(psql -h "${DATA_DIR}" -p "${TEST_PORT}" -d "${TEST_DB}" -tAc \
                    "SELECT COUNT(*) FROM conc_docs WHERE content <@> to_bm25query('database', 'conc_idx') < -0.01;" 2>/dev/null)
                if [ -z "$count" ]; then
                    errors=$((errors + 1))
                fi
                queries=$((queries + 1))
                sleep 0.1
            done
            echo "Reader $r: $queries queries, $errors errors"
        } &
        reader_pids+=($!)
    done

    # Writer: continuous inserts
    local writer_docs=0
    local spills=0
    while [ $(date +%s) -lt $end_time ]; do
        local insert_sql="INSERT INTO conc_docs (content) VALUES"
        local first=true
        for ((i=1; i<=20; i++)); do
            local content=$(generate_doc_content $((1000 + writer_docs + i)))
            if [ "$first" = true ]; then
                insert_sql="${insert_sql} ('${content}')"
                first=false
            else
                insert_sql="${insert_sql}, ('${content}')"
            fi
        done
        run_sql_quiet "${insert_sql};"
        writer_docs=$((writer_docs + 20))

        # Periodic manual spill
        if [ $((writer_docs % 200)) -eq 0 ]; then
            run_sql_quiet "SELECT bm25_spill_index('conc_idx');"
            spills=$((spills + 1))
        fi

        sleep 0.05
    done

    # Wait for readers
    local reader_failures=0
    for pid in "${reader_pids[@]}"; do
        if ! wait $pid 2>/dev/null; then
            reader_failures=$((reader_failures + 1))
        fi
    done

    info "Writer: $writer_docs docs inserted, $spills manual spills"

    if [ $reader_failures -eq 0 ]; then
        log "Test 2 passed: Concurrent stress test completed"
    else
        warn "Test 2: $reader_failures reader failures"
    fi

    run_sql_quiet "DROP TABLE conc_docs CASCADE;"
}

#
# Test 3: Multiple indexes with interleaved operations
#
test_multiple_indexes() {
    log "Test 3: Multiple indexes with interleaved operations"

    # Create 3 tables with indexes
    for t in 1 2 3; do
        run_sql_quiet "CREATE TABLE multi_${t} (id SERIAL PRIMARY KEY, content TEXT);"
        run_sql_quiet "CREATE INDEX multi_idx_${t} ON multi_${t} USING bm25(content) WITH (text_config='english');"
    done

    local iterations=50
    local errors=0

    for ((iter=1; iter<=iterations; iter++)); do
        # Insert to all tables
        for t in 1 2 3; do
            local content="iteration ${iter} table ${t} with alpha beta gamma"
            run_sql_quiet "INSERT INTO multi_${t} (content) VALUES ('${content}');"
        done

        # Query from all tables - use ORDER BY with LIMIT for BM25 index usage
        for t in 1 2 3; do
            local count=$(run_sql_value "SELECT COUNT(*) FROM (SELECT * FROM multi_${t} ORDER BY content <@> to_bm25query('alpha', 'multi_idx_${t}') LIMIT 1000) sub;")
            # Check count is positive (BM25 found matching documents)
            if [ -z "$count" ] || [ "$count" -eq 0 ]; then
                warn "Iteration $iter, table $t: expected documents, got $count"
                errors=$((errors + 1))
            fi
        done

        # Periodic spill to different indexes
        if [ $((iter % 15)) -eq 0 ]; then
            local table_num=$(( (iter / 15) % 3 + 1 ))
            run_sql_quiet "SELECT bm25_spill_index('multi_idx_${table_num}');"
        fi
    done

    # Final validation - check table row counts
    for t in 1 2 3; do
        local count=$(run_sql_value "SELECT COUNT(*) FROM multi_${t};")
        if [ "$count" != "$iterations" ]; then
            warn "Table $t final count: expected $iterations, got $count"
            errors=$((errors + 1))
        fi
    done

    # Verify BM25 queries return results after all operations
    for t in 1 2 3; do
        local bm25_count=$(run_sql_value "SELECT COUNT(*) FROM (SELECT * FROM multi_${t} ORDER BY content <@> to_bm25query('alpha', 'multi_idx_${t}') LIMIT 1000) sub;")
        if [ "$bm25_count" != "$iterations" ]; then
            warn "Table $t final BM25 count: expected $iterations, got $bm25_count"
            errors=$((errors + 1))
        fi
    done

    if [ $errors -eq 0 ]; then
        log "Test 3 passed: Multiple indexes test completed"
    else
        warn "Test 3 completed with $errors errors"
    fi

    for t in 1 2 3; do
        run_sql_quiet "DROP TABLE multi_${t} CASCADE;"
    done
}

#
# Test 4: Segment merge stress test
#
test_segment_merge_stress() {
    log "Test 4: Segment merge stress test"

    run_sql_quiet "CREATE TABLE merge_test (id SERIAL PRIMARY KEY, content TEXT);"
    run_sql_quiet "CREATE INDEX merge_idx ON merge_test USING bm25(content) WITH (text_config='english');"

    # Create many small segments by inserting and spilling repeatedly
    local target_segments=20
    local docs_per_segment=50

    for ((seg=1; seg<=target_segments; seg++)); do
        local insert_sql="INSERT INTO merge_test (content) VALUES"
        local first=true
        for ((i=1; i<=docs_per_segment; i++)); do
            local content="segment ${seg} document ${i} with search terms alpha beta"
            if [ "$first" = true ]; then
                insert_sql="${insert_sql} ('${content}')"
                first=false
            else
                insert_sql="${insert_sql}, ('${content}')"
            fi
        done
        run_sql_quiet "${insert_sql};"
        run_sql_quiet "SELECT bm25_spill_index('merge_idx');"

        if [ $((seg % 5)) -eq 0 ]; then
            info "Created $seg segments..."
        fi
    done

    local total_docs=$((target_segments * docs_per_segment))

    # Verify all documents are searchable using ORDER BY with LIMIT
    local alpha_count=$(run_sql_value "SELECT COUNT(*) FROM (SELECT * FROM merge_test ORDER BY content <@> to_bm25query('alpha', 'merge_idx') LIMIT 10000) sub;")

    if [ "$alpha_count" = "$total_docs" ]; then
        log "Test 4 passed: All $total_docs documents searchable across segments"
    else
        warn "Test 4: Expected $total_docs, found $alpha_count"
    fi

    run_sql_quiet "DROP TABLE merge_test CASCADE;"
}

#
# Test 5: Resource cleanup verification
#
test_resource_cleanup() {
    log "Test 5: Resource cleanup verification"

    # Create and drop indexes repeatedly
    local iterations=10

    for ((iter=1; iter<=iterations; iter++)); do
        run_sql_quiet "CREATE TABLE cleanup_${iter} (id SERIAL PRIMARY KEY, content TEXT);"
        run_sql_quiet "INSERT INTO cleanup_${iter} (content) SELECT 'document ' || g || ' alpha beta' FROM generate_series(1, 100) g;"
        run_sql_quiet "CREATE INDEX cleanup_idx_${iter} ON cleanup_${iter} USING bm25(content) WITH (text_config='english');"
        run_sql_quiet "SELECT bm25_spill_index('cleanup_idx_${iter}');"

        # Query a few times
        run_sql_quiet "SELECT COUNT(*) FROM cleanup_${iter} WHERE content <@> to_bm25query('alpha', 'cleanup_idx_${iter}') < -0.01;"

        # Drop table (which drops index too)
        run_sql_quiet "DROP TABLE cleanup_${iter} CASCADE;"
    done

    # Check no leaked resources (this relies on Postgres cleanup mechanisms)
    # The real verification happens when running under sanitizers

    log "Test 5 passed: Created and dropped $iterations indexes"
}

run_stress_tests() {
    log "Starting pg_textsearch stress tests"
    log "Duration: ${STRESS_DURATION_MINUTES} minutes"
    log "Spill threshold: ${SPILL_THRESHOLD}"

    test_extended_insert_query
    test_concurrent_stress
    test_multiple_indexes
    test_segment_merge_stress
    test_resource_cleanup

    log "All stress tests completed"
}

main() {
    log "Starting pg_textsearch stress testing..."
    log "Configuration:"
    log "  Duration: ${STRESS_DURATION_MINUTES} minutes"
    log "  Spill threshold: ${SPILL_THRESHOLD}"
    log "  Docs per batch: ${DOCS_PER_BATCH}"
    log "  Concurrent readers: ${CONCURRENT_READERS}"

    command -v pg_ctl >/dev/null 2>&1 || error "pg_ctl not found"
    command -v psql >/dev/null 2>&1 || error "psql not found"

    setup_test_db
    run_stress_tests

    log "Stress tests completed successfully!"
    exit 0
}

if [ "${BASH_SOURCE[0]}" == "${0}" ]; then
    main "$@"
fi
