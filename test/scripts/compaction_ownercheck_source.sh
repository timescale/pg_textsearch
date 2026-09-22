#!/bin/bash
#
# Verify that public compaction mutators reject nonowners before requesting a
# heavyweight relation lock, then recheck ownership after locking.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SOURCE_FILE="${SOURCE_FILE:-${REPO_ROOT}/src/access/compaction_api.c}"
BUILD_SOURCE="${BUILD_SOURCE:-${REPO_ROOT}/src/access/build.c}"
INSTALL_SQL="${REPO_ROOT}/sql/pg_textsearch--1.5.0-dev.sql"
UPGRADE_SQL="${REPO_ROOT}/sql/pg_textsearch--1.4.0--1.5.0-dev.sql"

open_body="$(
    sed -n '/^tp_open_bm25_index(Oid indexoid, LOCKMODE lockmode, bool need_owner)$/,/^}$/p' \
        "${SOURCE_FILE}"
)"

mapfile -t ownercheck_lines < <(
    grep -n "object_ownercheck(RelationRelationId, indexoid, GetUserId())" \
        <<<"${open_body}" |
        cut -d: -f1
)
relation_open_line="$(
    grep -n "index_rel = relation_open(indexoid, lockmode);" \
        <<<"${open_body}" |
        cut -d: -f1
)"

if [[ "${#ownercheck_lines[@]}" -ne 2 ]]; then
    echo "expected two compaction ownership checks, found ${#ownercheck_lines[@]}" >&2
    exit 1
fi

if [[ -z "${relation_open_line}" ||
      "${ownercheck_lines[0]}" -ge "${relation_open_line}" ||
      "${ownercheck_lines[1]}" -le "${relation_open_line}" ]]; then
    echo "compaction ownership checks must bracket relation_open" >&2
    exit 1
fi

check_spill_policy_order() {
    local function_name="$1"
    local function_body
    local acquire_line
    local spill_line
    local release_line
    local policy_line

    function_body="$(
        sed -n "/^${function_name}(/,/^}$/p" "${BUILD_SOURCE}"
    )"
    acquire_line="$(
        grep -n 'tp_acquire_index_lock(index_state, LW_EXCLUSIVE)' \
            <<<"${function_body}" | head -1 | cut -d: -f1 || true
    )"
    spill_line="$(
        grep -n 'tp_do_spill(index_state' <<<"${function_body}" |
            head -1 | cut -d: -f1 || true
    )"
    release_line="$(
        grep -n 'tp_release_index_lock(index_state)' <<<"${function_body}" |
            head -1 | cut -d: -f1 || true
    )"
    policy_line="$(
        grep -n 'tp_apply_compaction_policy' <<<"${function_body}" |
            head -1 | cut -d: -f1 || true
    )"

    if [[ -z "${acquire_line}" || -z "${spill_line}" ||
          -z "${release_line}" || -z "${policy_line}" ||
          "${acquire_line}" -ge "${spill_line}" ||
          "${spill_line}" -ge "${release_line}" ||
          "${release_line}" -ge "${policy_line}" ]]; then
        echo "${function_name} must release the index lock before policy" >&2
        exit 1
    fi
}

check_spill_policy_order tp_spill_memtable_if_needed_internal
check_spill_policy_order tp_auto_spill_if_needed
check_spill_policy_order tp_spill_memtable

check_compaction_lock_order() {
    local function_name="$1"
    local function_body
    local maintenance_line
    local index_lock_line

    function_body="$(
        sed -n "/^${function_name}(PG_FUNCTION_ARGS)$/,/^}$/p" \
            "${SOURCE_FILE}"
    )"
    maintenance_line="$(
        grep -n 'tp_compaction_lock(index_rel)' \
            <<<"${function_body}" | head -1 | cut -d: -f1 || true
    )"
    index_lock_line="$(
        grep -n 'tp_acquire_index_lock(index_state' \
            <<<"${function_body}" | head -1 | cut -d: -f1
    )"

    if [[ -z "${maintenance_line}" || -z "${index_lock_line}" ||
          "${maintenance_line}" -ge "${index_lock_line}" ]]; then
        echo "${function_name} must acquire maintenance before the index lock" >&2
        exit 1
    fi
}

check_compaction_lock_order tp_compact_index
check_compaction_lock_order tp_compact_index_step
current_step_body="$(
    sed -n '/^tp_compact_index_step_if_current(PG_FUNCTION_ARGS)$/,/^}$/p' \
        "${SOURCE_FILE}"
)"
current_maintenance_line="$(
    grep -n 'tp_try_compaction_lock(index_rel)' <<<"${current_step_body}" |
        cut -d: -f1 || true
)"
current_decline_line="$(
    grep -n 'PG_RETURN_BOOL(false)' <<<"${current_step_body}" |
        tail -1 | cut -d: -f1 || true
)"
current_revalidate_line="$(
    grep -n 'tp_open_current_bm25_target(' <<<"${current_step_body}" |
        tail -1 | cut -d: -f1
)"
current_nolock_line="$(
    grep -n '&target, NoLock, true, true' <<<"${current_step_body}" |
        cut -d: -f1
)"
current_index_lock_line="$(
    grep -n 'tp_acquire_index_lock(index_state, LW_EXCLUSIVE)' \
        <<<"${current_step_body}" | cut -d: -f1
)"
if [[ -z "${current_maintenance_line}" || -z "${current_decline_line}" ||
      -z "${current_revalidate_line}" || -z "${current_nolock_line}" ||
      -z "${current_index_lock_line}" ||
      "${current_maintenance_line}" -ge "${current_decline_line}" ||
      "${current_decline_line}" -ge "${current_revalidate_line}" ||
      "${current_revalidate_line}" -gt "${current_nolock_line}" ||
      "${current_nolock_line}" -ge "${current_index_lock_line}" ]]; then
    echo "managed compaction must conditionally admit then revalidate" >&2
    exit 1
fi

for sql_file in "${INSTALL_SQL}" "${UPGRADE_SQL}"; do
    for function_name in \
        bm25_test_hold_index_lock \
        bm25_test_exclusive_waiters; do
        function_mentions="$(
            grep -Fc "@extschema@.${function_name}" "${sql_file}"
        )"
        if ! grep -Fq "CREATE FUNCTION @extschema@.${function_name}" \
                "${sql_file}" ||
           [ "${function_mentions}" -lt 2 ]; then
            echo "${sql_file} lacks ${function_name} creation or revoke" >&2
            exit 1
        fi
    done
done

echo "Compaction ownership and lock ordering passed"
