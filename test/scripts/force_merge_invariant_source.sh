#!/bin/bash
#
# Verify that force-merge execution invariants remain enforced when PostgreSQL
# assertions are compiled out. The impossible states are not safely injectable
# through SQL without adding a production-only corruption hook.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SOURCE_FILE="${REPO_ROOT}/src/segment/merge.c"

force_merge_body="$(
    sed -n '/^tp_force_merge_all(Relation index)/,$p' "${SOURCE_FILE}"
)"

require_text() {
    local expected="$1"

    if ! grep -Fq "${expected}" <<<"${force_merge_body}"; then
        echo "missing force-merge production invariant check: ${expected}" >&2
        exit 1
    fi
}

require_text "ERRCODE_INTERNAL_ERROR"
require_text "internal force-merge invariant failure for "
require_text "planner returned no executable "
require_text "merge with %u segments remaining"
require_text "planned merge of level %u "
require_text "failed with %u segments remaining"
require_text "%u segments remain after "
require_text "if (remaining_segment_count > 1)"
require_text "if (final_segment_count > 1)"
require_text "Assert(action == TP_FORCE_MERGE_LEVEL)"
require_text "Assert(remaining_segment_count <= 1)"
require_text "Assert(final_segment_count <= 1)"

error_code_count="$(
    grep -Fc "errcode(ERRCODE_INTERNAL_ERROR)" <<<"${force_merge_body}"
)"
if [[ "${error_code_count}" -ne 3 ]]; then
    echo "expected three force-merge internal error codes, found ${error_code_count}" >&2
    exit 1
fi

error_count="$(
    grep -Fc "internal force-merge invariant failure for " \
        <<<"${force_merge_body}"
)"
if [[ "${error_count}" -ne 3 ]]; then
    echo "expected three force-merge production invariant errors, found ${error_count}" >&2
    exit 1
fi

echo "Force-merge production invariant checks passed"
