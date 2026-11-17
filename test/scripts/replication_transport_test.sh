#!/usr/bin/env bash

# Exercise transport configuration and cleanup without a PostgreSQL install.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

fail() {
    echo "replication transport test failed: $*" >&2
    exit 1
}

run_case() {
    local mode=$1
    CASE_DIR=$2
    PRIMARY_PORT=55438
    STANDBY_PORT=55439
    STANDBY2_PORT=55440
    PRIMARY_DIR="${CASE_DIR}/primary"
    STANDBY_DIR="${CASE_DIR}/standby"
    STANDBY2_DIR="${CASE_DIR}/standby2"
    TEST_DB=transport_test
    REPL_SOCKET_ONLY=0
    REPL_SOCKET_DIR="${CASE_DIR}/caller-sockets"
    REPL_HOST=
    mkdir -p "${REPL_SOCKET_DIR}"
    if [ "$mode" = private ]; then
        REPL_SOCKET_ONLY=1
        # Socket-only mode must override even inherited TCP settings.
        REPL_HOST=127.0.0.1
        REPL_SOCKET_DIR=
    elif [ "$mode" = tcp ]; then
        REPL_HOST=127.0.0.1
        REPL_SOCKET_DIR=
    fi

    # Minimal stand-ins for the backend tools: retain the generated
    # configuration and model node start/stop, not database behavior.
    pg_config() { echo "${CASE_DIR}/no-extensions"; }
    initdb() {
        mkdir -p "$2"
        touch "$2/postgresql.conf" "$2/pg_hba.conf"
        printf '%s\n' "$PGHOST" > "${CASE_DIR}/host"
    }
    pg_ctl() {
        if [ "$1" = start ]; then
            touch "$3/postmaster.pid"
        else
            rm -f "$3/postmaster.pid"
        fi
    }
    createdb() { return "${CREATEDB_STATUS:-0}"; }
    psql() { echo t; }
    pg_basebackup() {
        mkdir -p "$2"
        cp "${PRIMARY_DIR}/postgresql.conf" "$2/postgresql.conf"
    }

    # shellcheck source=replication_lib.sh
    source "${SCRIPT_DIR}/replication_lib.sh"
    trap repl_cleanup EXIT
    setup_primary
    setup_standby
    setup_standby2

    local addresses=localhost
    if [ "$mode" = private ]; then
        addresses=
        [[ "$PGHOST" == /tmp/pgts-repl.* ]] ||
            fail "private socket directory is not short and absolute"
        [ -d "$PGHOST" ] || fail "private socket directory is missing"
        [ "$PGHOST" = "$REPL_OWNED_SOCKET_DIR" ] ||
            fail "clients do not use the owned socket directory"
    else
        [ -z "$REPL_OWNED_SOCKET_DIR" ] ||
            fail "non-private mode allocated a socket directory"
        [ "$PGHOST" = "${REPL_HOST:-${REPL_SOCKET_DIR}}" ] ||
            fail "existing transport settings were not preserved"
    fi
    for dir in "$PRIMARY_DIR" "$STANDBY_DIR" "$STANDBY2_DIR"; do
        grep -Fxq "listen_addresses = '${addresses}'" \
            "$dir/postgresql.conf" || fail "incorrect TCP listeners"
        grep -Fxq "unix_socket_directories = '${REPL_SOCKET_DIR}'" \
            "$dir/postgresql.conf" || fail "incorrect socket directory"
    done
}

if [ "${1:-}" = --case ]; then
    run_case "$2" "$3"
    exit 0
fi

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
previous_host=
for mode in private private default tcp; do
    case_dir=$(mktemp -d "${TEST_DIR}/case.XXXXXX")
    bash "$0" --case "$mode" "$case_dir"
    host=$(<"${case_dir}/host")
    [ ! -d "${case_dir}/primary" ] || fail "node was not cleaned up"
    [ -d "${case_dir}/caller-sockets" ] ||
        fail "cleanup removed a caller-owned directory"
    if [ "$mode" = private ]; then
        [ ! -d "$host" ] || fail "private socket directory leaked"
        [ "$host" != "$previous_host" ] ||
            fail "test runs shared a socket directory"
        previous_host=$host
    fi
done

# A setup failure must retain its status and remove the private directory.
case_dir=$(mktemp -d "${TEST_DIR}/failure.XXXXXX")
status=0
CREATEDB_STATUS=7 bash "$0" --case private "$case_dir" || status=$?
[ "$status" -eq 7 ] || fail "setup failure status was lost"
host=$(<"${case_dir}/host")
[ ! -d "$host" ] || fail "failed setup leaked its socket directory"
[ ! -d "${case_dir}/primary" ] || fail "failed setup leaked its node"

echo "replication transport tests passed"
