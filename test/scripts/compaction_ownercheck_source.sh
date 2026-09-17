#!/bin/bash
#
# Verify that public compaction mutators reject nonowners before requesting a
# heavyweight relation lock, then recheck ownership after locking.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SOURCE_FILE="${REPO_ROOT}/src/access/compaction_api.c"
BUILD_SOURCE="${REPO_ROOT}/src/access/build.c"

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

release_line="$(
    grep -n 'tp_release_index_lock(index_state)' \
        "${BUILD_SOURCE}" | head -1 | cut -d: -f1
)"
policy_line="$(
    grep -n 'tp_apply_compaction_policy' \
        "${BUILD_SOURCE}" | tail -1 | cut -d: -f1 || true
)"
if [[ -z "${release_line}" || -z "${policy_line}" ||
      "${release_line}" -ge "${policy_line}" ]]; then
    echo "spill policy must run after releasing the index lock" >&2
    exit 1
fi

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

echo "Compaction ownership and lock ordering passed"
