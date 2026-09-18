#!/bin/bash
#
# Guard atomic graph snapshot ordering and standalone source ownership.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
GRAPH_SOURCE="${REPO_ROOT}/src/segment/graph_snapshot.c"
QUERY_SOURCE="${REPO_ROOT}/src/types/query.c"
SCORING_SOURCE="${REPO_ROOT}/src/scoring/bm25.c"
CHAIN_SOURCE="${REPO_ROOT}/src/memtable/chain_source.c"

graph_body="$(
    sed -n '/^tp_segment_graph_snapshot_create(Relation index)/,/^}/p' \
        "${GRAPH_SOURCE}"
)"
query_body="$(
    sed -n '/^bm25_text_bm25query_score(PG_FUNCTION_ARGS)/,/^}/p' \
        "${QUERY_SOURCE}"
)"
standalone_open_body="$(
    sed -n '/^tp_standalone_sources_open(/,/^}/p' "${QUERY_SOURCE}"
)"
scoring_body="$(
    sed -n '/^tp_score_documents(/,/^}/p' "${SCORING_SOURCE}"
)"
bounded_source_body="$(
    sed -n '/^tp_memtable_chain_source_create_bounded(/,/^}/p' \
        "${CHAIN_SOURCE}"
)"

if [[ -z "${graph_body}" || -z "${query_body}" ||
      -z "${standalone_open_body}" || -z "${scoring_body}" ||
      -z "${bounded_source_body}" ]]; then
    echo "could not extract graph, scoring, or bounded source body" >&2
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
memtable_snapshot_line="$(
    unique_line "${graph_body}" "tp_memtable_chain_snapshot_capture("
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

if [[ "${root_complete_line}" -ge "${memtable_snapshot_line}" ||
      "${memtable_snapshot_line}" -ge "${before_unlock_line}" ||
      "${before_unlock_line}" -ge "${unlock_line}" ||
      "${unlock_line}" -ge "${after_unlock_line}" ]]; then
    echo "read snapshot does not capture roots and memtable before unlock" >&2
    exit 1
fi

if [[ "$(grep -Fc "tp_standalone_sources_open(" <<<"${query_body}")" -ne 2 ]]; then
    echo "standalone scoring must use the common source opener twice" >&2
    exit 1
fi

if ! grep -Fq "RecoveryInProgress()" <<<"${standalone_open_body}" ||
   ! grep -Fq "tp_segment_graph_snapshot_create(" \
        <<<"${standalone_open_body}" ||
   ! grep -Fq "tp_memtable_chain_source_create_bounded(" \
        <<<"${standalone_open_body}" ||
   ! grep -Fq "tp_memtable_source_create_for_read(" \
        <<<"${standalone_open_body}"; then
    echo "standalone opener does not preserve primary and recovery paths" >&2
    exit 1
fi

mapfile -t standalone_snapshot_lines < <(
    grep -nF "tp_segment_graph_snapshot_create(" \
        <<<"${standalone_open_body}" | cut -d: -f1
)
standalone_bounded_line="$(
    unique_line "${standalone_open_body}" \
        "tp_memtable_chain_source_create_bounded("
)"
standalone_primary_line="$(
    unique_line "${standalone_open_body}" \
        "tp_memtable_source_create_for_read("
)"
if [[ "${#standalone_snapshot_lines[@]}" -ne 2 ||
      "${standalone_snapshot_lines[0]}" -ge "${standalone_bounded_line}" ||
      "${standalone_primary_line}" -ge "${standalone_snapshot_lines[1]}" ]]; then
    echo "standalone source ordering changed on recovery or primary" >&2
    exit 1
fi

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
    echo "standalone scoring releases the primary source lock too early" >&2
    exit 1
fi

if ! grep -Fq "RecoveryInProgress()" <<<"${scoring_body}" ||
   ! grep -Fq "tp_memtable_chain_source_create_bounded(" \
        <<<"${scoring_body}" ||
   ! grep -Fq "&snapshot->memtable" <<<"${scoring_body}"; then
    echo "ranked recovery scoring does not use the common memtable snapshot" >&2
    exit 1
fi

if grep -Fq "tp_get_metapage" <<<"${bounded_source_body}" ||
   ! grep -Fq "tp_memtable_chain_source_create_internal(" \
        <<<"${bounded_source_body}"; then
    echo "bounded chain source rereads the metapage or duplicates ingestion" >&2
    exit 1
fi

echo "Atomic root and memtable snapshot source guards passed"
