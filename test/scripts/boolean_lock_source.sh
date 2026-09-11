#!/bin/bash
#
# Guard that Boolean evaluation does not retain the per-index LWLock while it
# evaluates immutable segments or writes the complete result set.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SCAN_SOURCE="${REPO_ROOT}/src/access/scan.c"
BOOLEAN_SOURCE="${REPO_ROOT}/src/access/boolean.c"

gettuple_body="$(
    sed -n '/^tp_gettuple(IndexScanDesc scan, ScanDirection dir)/,/^}/p' \
        "${SCAN_SOURCE}"
)"
execute_body="$(
    sed -n '/^tp_boolean_execute(/,/^}/p' "${BOOLEAN_SOURCE}"
)"

if [[ -z "${gettuple_body}" || -z "${execute_body}" ]]; then
    echo "could not extract Boolean scan function bodies" >&2
    exit 1
fi

if grep -Fq "tp_acquire_index_lock" <<<"${gettuple_body}"; then
    echo "Boolean scan holds the per-index LWLock around full execution" >&2
    exit 1
fi

if ! grep -Fq "tp_boolean_segment_snapshot_create" <<<"${execute_body}" ||
    ! grep -Fq "tp_source_close(source)" <<<"${execute_body}" ||
    ! grep -Fq "tp_release_index_lock(index_state)" <<<"${execute_body}"; then
    echo "Boolean execution does not snapshot mutable sources before unlocking" >&2
    exit 1
fi

unique_line() {
    local pattern=$1
    local lines

    lines="$(grep -nF "${pattern}" <<<"${execute_body}" || true)"
    if [[ "$(wc -l <<<"${lines}")" -ne 1 || -z "${lines}" ]]; then
        echo "expected exactly one Boolean execution match for: ${pattern}" >&2
        exit 1
    fi
    cut -d: -f1 <<<"${lines}"
}

acquire_line="$(unique_line "tp_acquire_index_lock(index_state, LW_SHARED)")"
snapshot_line="$(unique_line "tp_boolean_segment_snapshot_create")"
close_line="$(unique_line "tp_source_close(source)")"
release_line="$(unique_line "tp_release_index_lock(index_state)")"
candidate_write_line="$(
    unique_line "tp_boolean_write_candidate(&candidate, &writer)"
)"
segment_write_line="$(unique_line "tp_boolean_write_segment")"

if [[ "${acquire_line}" -ge "${snapshot_line}" ||
      "${snapshot_line}" -ge "${close_line}" ||
      "${close_line}" -ge "${release_line}" ||
      "${release_line}" -ge "${candidate_write_line}" ||
      "${release_line}" -ge "${segment_write_line}" ]]; then
    echo "Boolean candidate evaluation still runs under the per-index LWLock" >&2
    exit 1
fi

echo "Boolean lock lifetime source guards passed"
