#!/bin/bash
#
# pinned_horizon.sh — rerun the SQL regression suite against a primary
# whose reclaim horizon is pinned by a hot-standby-feedback standby
# holding an open snapshot.
#
# A single-node `make installcheck` never pins the horizon, so it cannot
# catch an assertion on counts taken after a DELETE, VACUUM or REINDEX
# that is missing a horizon_pinned() guard.  See the docblock in
# test/sql/memtable_reclaim.sql for why those counts move.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

PRIMARY_PORT=55470
STANDBY_PORT=55471
PRIMARY_DIR="${SCRIPT_DIR}/../tmp_pinned_horizon_primary"
STANDBY_DIR="${SCRIPT_DIR}/../tmp_pinned_horizon_standby"
TEST_DB=pinned_horizon_probe
REGRESS_DB=contrib_regression
SLOT_NAME=pinned_horizon_slot

# pg_regress runs the tests, which CREATE and DROP the extension
# themselves, so the probe database must not have it pre-created.
SETUP_CREATE_EXTENSION=0

# shellcheck source=test/scripts/replication_lib.sh
source "${SCRIPT_DIR}/replication_lib.sh"

trap repl_cleanup EXIT INT TERM

main() {
    check_required_tools

    setup_primary

    log "Creating physical replication slot '${SLOT_NAME}'..."
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c \
        "SELECT pg_create_physical_replication_slot('${SLOT_NAME}');" \
        >/dev/null || error "could not create replication slot"

    setup_standby

    log "Enabling hot_standby_feedback on the standby..."
    cat >> "${STANDBY_DIR}/postgresql.conf" <<EOF
hot_standby_feedback = on
primary_slot_name = '${SLOT_NAME}'
EOF
    # Both settings are PGC_SIGHUP, so a reload is enough.
    pg_ctl reload -D "${STANDBY_DIR}" >/dev/null ||
        error "could not reload standby"

    pin_horizon
    assert_horizon_pinned
    run_regression_suite
}

# Current xmin held by the standby's replication slot, if any.
slot_xmin() {
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c \
        "SELECT xmin::text::bigint FROM pg_replication_slots
          WHERE slot_name = '${SLOT_NAME}';" 2>/dev/null
}

# Hold a REPEATABLE READ snapshot open on the standby for the rest of the
# run; feedback propagates its xmin to the primary's slot.
pin_horizon() {
    log "Opening a long-lived standby snapshot..."
    long_lived_open "${STANDBY_PORT}"
    long_lived_query "BEGIN ISOLATION LEVEL REPEATABLE READ;" >/dev/null
    # Force the snapshot to be taken and registered now.
    long_lived_query "SELECT count(*) FROM pg_class;" >/dev/null

    log "Waiting for feedback to reach the primary's slot..."
    local waited=0
    while [ "${waited}" -lt 60 ]; do
        local xmin
        xmin=$(psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c \
            "SELECT xmin FROM pg_replication_slots
              WHERE slot_name = '${SLOT_NAME}';" 2>/dev/null)
        if [ -n "${xmin}" ]; then
            log "Slot xmin is ${xmin} after ${waited}s"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done
    error "standby feedback never set an xmin on the slot"
}

# Fail loudly if the horizon is not actually held back; otherwise this
# script silently degrades into a slower duplicate of installcheck.
assert_horizon_pinned() {
    log "Asserting the primary's reclaim horizon is pinned..."

    # Burn a couple of xids, then confirm the horizon did not follow.
    # Comparing against the current snapshot would be wrong: the frozen
    # xmin equals the next unassigned xid, which the first probe takes.
    local slot_xmin_before slot_xmin_after new_xid
    slot_xmin_before=$(slot_xmin)
    psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c \
        "SELECT pg_current_xact_id();" >/dev/null
    new_xid=$(psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c \
        "SELECT pg_current_xact_id()::text::bigint;")
    slot_xmin_after=$(slot_xmin)

    if [ -z "${slot_xmin_before}" ] || [ -z "${slot_xmin_after}" ] ||
       [ -z "${new_xid}" ]; then
        error "could not read slot xmin / current xid"
    fi
    if [ "${slot_xmin_after}" != "${slot_xmin_before}" ]; then
        error "horizon advanced from ${slot_xmin_before} to \
${slot_xmin_after} while the standby snapshot was open"
    fi
    if [ "${slot_xmin_after}" -ge "${new_xid}" ]; then
        error "slot xmin ${slot_xmin_after} does not trail new xid \
${new_xid} -- horizon is not pinned"
    fi
    log "Horizon held at ${slot_xmin_after} while xids advanced to ${new_xid}."

    # If horizon_pinned() stops detecting this topology, every guard
    # silently stops working -- so assert the predicate itself.
    local detected
    detected=$(psql -p "${PRIMARY_PORT}" -d "${TEST_DB}" -tA -c \
        "SELECT EXISTS (
                    SELECT 1
                    FROM pg_stat_activity
                    WHERE pid <> pg_backend_pid()
                      AND backend_xmin IS NOT NULL
                      AND (datname = current_database()
                           OR backend_type = 'walsender')
                )
             OR EXISTS (SELECT 1 FROM pg_replication_slots
                         WHERE xmin IS NOT NULL)
             OR EXISTS (SELECT 1 FROM pg_prepared_xacts);")
    if [ "${detected}" != "t" ]; then
        error "horizon_pinned() would return false in this pinned \
topology -- the test guards are no longer effective"
    fi

    log "Horizon confirmed pinned and detectable."
}

run_regression_suite() {
    local pg_regress regress_list regress_dir
    regress_dir="$(dirname "$(pg_config --pgxs)")/../../src/test/regress"
    pg_regress="${regress_dir}/pg_regress"
    [ -x "${pg_regress}" ] || error "pg_regress not found at ${pg_regress}"

    regress_list=$(make -C "${SRC_DIR}" -s --no-print-directory \
        print-regress) || error "could not read REGRESS list"
    [ -n "${regress_list}" ] || error "REGRESS list is empty"

    createdb -p "${PRIMARY_PORT}" "${REGRESS_DB}" ||
        error "could not create ${REGRESS_DB}"

    log "Running regression suite against the pinned primary..."
    # Tests include files by repo-root-relative path (\i test/sql/...),
    # so pg_regress must run from the repo root, as installcheck does.
    cd "${SRC_DIR}" || error "could not cd to ${SRC_DIR}"
    # shellcheck disable=SC2086
    if "${pg_regress}" --use-existing --host=/tmp \
        --port="${PRIMARY_PORT}" --dbname="${REGRESS_DB}" \
        --inputdir="${SRC_DIR}/test" --outputdir="${SRC_DIR}/test" \
        ${regress_list}; then
        log "All regression tests passed with the horizon pinned."
        return 0
    fi

    warn "Regression failures under a pinned horizon."
    warn "An assertion on counts taken after a DELETE, VACUUM or"
    warn "REINDEX likely needs a horizon_pinned() guard; see"
    warn "test/sql/memtable_reclaim.sql for the pattern."
    if [ -f "${SRC_DIR}/test/regression.diffs" ]; then
        echo "=== regression.diffs ==="
        cat "${SRC_DIR}/test/regression.diffs"
    fi
    error "pinned-horizon regression run failed"
}

main "$@"
