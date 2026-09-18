#!/bin/bash
#
# Guard full-fork dead-memtable reclaim ownership and lock lifetime.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VACUUM_SOURCE="${REPO_ROOT}/src/access/vacuum.c"

cleanup_body="$(
    sed -n '/^tp_vacuumcleanup(/,/^}/p' "${VACUUM_SOURCE}"
)"

if [[ -z "${cleanup_body}" ]]; then
    echo "could not extract tp_vacuumcleanup" >&2
    exit 1
fi

unique_line() {
    local pattern=$1
    local lines

    lines="$(grep -nF "${pattern}" <<<"${cleanup_body}" || true)"
    if [[ "$(wc -l <<<"${lines}")" -ne 1 || -z "${lines}" ]]; then
        echo "expected exactly one cleanup match for: ${pattern}" >&2
        exit 1
    fi
    cut -d: -f1 <<<"${lines}"
}

reclaim_line="$(unique_line "tp_reclaim_dead_memtable_pages(")"
first_unlock_line="$(
    grep -nF "tp_compaction_unlock(info->index)" <<<"${cleanup_body}" |
        head -1 | cut -d: -f1
)"

if [[ -z "${first_unlock_line}" ||
      "${first_unlock_line}" -le "${reclaim_line}" ]]; then
    echo "VACUUM releases maintenance before full-fork memtable reclaim" >&2
    exit 1
fi

pre_reclaim="$(
    sed -n '/tp_segment_graph_snapshot_free(segment_snapshot)/,/tp_reclaim_dead_memtable_pages(/p' \
        <<<"${cleanup_body}"
)"
if grep -Eq 'tp_acquire_index_lock|tp_release_index_lock' \
        <<<"${pre_reclaim}"; then
    echo "VACUUM holds the per-index LWLock during full-fork reclaim" >&2
    exit 1
fi

echo "VACUUM full-fork reclaim source guards passed"
