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

if ! grep -Fq "tp_memtable_chain_snapshot_capture" <<<"${execute_body}" ||
    ! grep -Fq "tp_boolean_segment_snapshot_create" <<<"${execute_body}" ||
    ! grep -Fq "tp_release_index_lock(index_state)" <<<"${execute_body}"; then
    echo "Boolean execution does not snapshot mutable sources before unlocking" >&2
    exit 1
fi

if grep -Eq \
    "tp_memtable_source_create_for_read|tp_source_get_postings|tp_source_foreach_document" \
    <<<"${execute_body}"; then
    echo "Boolean execution still materializes the memtable source" >&2
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
memtable_snapshot_line="$(
    unique_line "tp_memtable_chain_snapshot_capture"
)"
snapshot_line="$(unique_line "tp_boolean_segment_snapshot_create")"
release_line="$(unique_line "tp_release_index_lock(index_state)")"
memtable_write_line="$(
    unique_line "tp_boolean_write_memtable_snapshot"
)"
segment_write_line="$(unique_line "tp_boolean_write_segment")"

if [[ "${acquire_line}" -ge "${memtable_snapshot_line}" ||
      "${memtable_snapshot_line}" -ge "${snapshot_line}" ||
      "${snapshot_line}" -ge "${release_line}" ||
      "${release_line}" -ge "${memtable_write_line}" ||
      "${release_line}" -ge "${segment_write_line}" ]]; then
    echo "Boolean candidate evaluation still runs under the per-index LWLock" >&2
    exit 1
fi

if ! grep -Fq "stream->state.term.iterator.force_copy = true" \
        "${BOOLEAN_SOURCE}" ||
    ! grep -Fq "cursors[i].iterator.force_copy = true" \
        "${BOOLEAN_SOURCE}"; then
    echo "Boolean exact-term iterators can retain buffer LWLocks" >&2
    exit 1
fi

echo "Boolean lock lifetime source guards passed"
