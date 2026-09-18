#!/bin/bash
#
# Guard atomic graph snapshot ordering and standalone source ownership.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
GRAPH_SOURCE="${REPO_ROOT}/src/segment/graph_snapshot.c"
QUERY_SOURCE="${REPO_ROOT}/src/types/query.c"

graph_body="$(
    sed -n '/^tp_segment_graph_snapshot_create(Relation index)/,/^}/p' \
        "${GRAPH_SOURCE}"
)"
query_body="$(
    sed -n '/^bm25_text_bm25query_score(PG_FUNCTION_ARGS)/,/^}/p' \
        "${QUERY_SOURCE}"
)"

if [[ -z "${graph_body}" || -z "${query_body}" ]]; then
    echo "could not extract graph snapshot or standalone scoring body" >&2
    exit 1
fi

unique_line() {
    local body=$1
    local pattern=$2
    local lines

    lines="$(grep -nF "${pattern}" <<<"${body}" || true)"
    if [[ "$(wc -l <<<"${lines}")" -ne 1 || -z "${lines}" ]]; then
        echo "expected exactly one match for: ${pattern}" >&2
        exit 1
    fi
    cut -d: -f1 <<<"${lines}"
}

root_complete_line="$(
    unique_line "${graph_body}" \
        "snapshot->level_offsets[TP_MAX_LEVELS] = snapshot->root_count"
)"
before_unlock_line="$(
    unique_line "${graph_body}" \
        "tp_debug_segment_graph_snapshot_pause_before_unlock_ms"
)"
unlock_line="$(unique_line "${graph_body}" "UnlockReleaseBuffer(buffer)")"
after_unlock_line="$(
    unique_line "${graph_body}" \
        "tp_debug_segment_graph_snapshot_pause_ms"
)"

if [[ "${root_complete_line}" -ge "${before_unlock_line}" ||
      "${before_unlock_line}" -ge "${unlock_line}" ||
      "${unlock_line}" -ge "${after_unlock_line}" ]]; then
    echo "segment graph snapshot pauses or unlocks before root collection" >&2
    exit 1
fi

mapfile -t source_lines < <(
    grep -nF "tp_memtable_source_create_for_read(" <<<"${query_body}" |
        cut -d: -f1
)
mapfile -t snapshot_lines < <(
    grep -nF "tp_segment_graph_snapshot_create(" <<<"${query_body}" |
        cut -d: -f1
)

if [[ "${#source_lines[@]}" -ne 2 ||
      "${#snapshot_lines[@]}" -ne 2 ]]; then
    echo "standalone scoring must have two source/snapshot acquisition pairs" >&2
    exit 1
fi
for i in 0 1; do
    if [[ "${source_lines[$i]}" -ge "${snapshot_lines[$i]}" ]]; then
        echo "standalone scoring snapshots segments before opening memtable source" >&2
        exit 1
    fi
done

success_cleanup="$(
    sed -n '/\/\* Clean up \*\//,/PG_CATCH()/p' <<<"${query_body}"
)"
catch_cleanup="$(
    sed -n '/PG_CATCH()/,/PG_RE_THROW()/p' <<<"${query_body}"
)"

success_snapshot_free="$(
    unique_line "${success_cleanup}" \
        "tp_segment_graph_snapshot_free(segment_snapshot)"
)"
success_source_close="$(
    unique_line "${success_cleanup}" "tp_source_close(memtable_src)"
)"
catch_snapshot_free="$(
    unique_line "${catch_cleanup}" \
        "tp_segment_graph_snapshot_free(segment_snapshot)"
)"
catch_source_close="$(
    unique_line "${catch_cleanup}" "tp_source_close(memtable_src)"
)"

if [[ "${success_snapshot_free}" -ge "${success_source_close}" ||
      "${catch_snapshot_free}" -ge "${catch_source_close}" ]]; then
    echo "standalone scoring releases the memtable source lock too early" >&2
    exit 1
fi

echo "Graph snapshot and standalone source ordering guards passed"
