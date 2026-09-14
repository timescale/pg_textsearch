#!/bin/bash
#
# Verify that public compaction mutators reject nonowners before requesting a
# heavyweight relation lock, then recheck ownership after locking.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SOURCE_FILE="${REPO_ROOT}/src/access/compaction_api.c"

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

echo "Compaction ownership check ordering passed"
