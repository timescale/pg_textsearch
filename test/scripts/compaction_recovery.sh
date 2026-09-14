#!/bin/bash
#
# Verify that per-index compaction mutators reject execution on a
# physical standby before opening or changing the index.

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

main() {
    log "Starting per-index compaction recovery-guard test..."
    check_required_tools
    setup_primary

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

    log "Per-index compaction recovery-guard test PASSED"
}

main "$@"
