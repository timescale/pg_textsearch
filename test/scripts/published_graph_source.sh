#!/bin/bash
#
# Guard published segment-graph readers against lazy link traversal.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DUMP_SOURCE="${REPO_ROOT}/src/debug/dump.c"
BUILD_SOURCE="${REPO_ROOT}/src/access/build.c"
VACUUM_SOURCE="${REPO_ROOT}/src/access/vacuum.c"

extract_body() {
    local source=$1
    local function_name=$2
    local body

    body="$(sed -n "/^${function_name}(/,/^}/p" "${source}")"
    if [[ -z "${body}" ]]; then
        echo "could not extract ${function_name} from ${source}" >&2
        exit 1
    fi
    printf '%s\n' "${body}"
}

assert_snapshot_owner() {
    local source=$1
    local function_name=$2
    local body

    body="$(extract_body "${source}" "${function_name}")"
    if ! grep -Fq "tp_segment_graph_snapshot_create(" <<<"${body}"; then
        echo "${function_name} must acquire a bounded segment graph snapshot" >&2
        exit 1
    fi
}

assert_no_lazy_graph_walk() {
    local source=$1
    local function_name=$2
    local body

    body="$(extract_body "${source}" "${function_name}")"
    if grep -Eq \
        'level_heads\[|header->next_segment|tp_segment_read_next\(' \
        <<<"${body}"; then
        echo "${function_name} still lazily traverses the published graph" >&2
        exit 1
    fi
}

assert_recyclable_suffix_truncation() {
    local body

    body="$(extract_body "${BUILD_SOURCE}" "tp_truncate_dead_pages")"
    if ! grep -Fq "tp_page_is_recyclable(page)" <<<"${body}" ||
       ! grep -Fq "truncate_to - 1" <<<"${body}" ||
       ! grep -Fq "RelationTruncate(index, truncate_to)" <<<"${body}" ||
       grep -Fq "tp_segment_graph_snapshot_create(" <<<"${body}" ||
       grep -Fq "tp_tombstone_max_used_block(" <<<"${body}"; then
        echo "force-merge truncation must remove only a recyclable EOF suffix" \
            >&2
        exit 1
    fi
}

assert_snapshot_owner "${DUMP_SOURCE}" "tp_summarize_index_to_output"
assert_snapshot_owner "${DUMP_SOURCE}" "tp_dump_index_to_output"
assert_snapshot_owner "${DUMP_SOURCE}" "tp_debug_pageviz_to_file"
assert_snapshot_owner "${VACUUM_SOURCE}" "tp_bulkdelete"
assert_snapshot_owner "${VACUUM_SOURCE}" "tp_vacuumcleanup"

assert_no_lazy_graph_walk "${DUMP_SOURCE}" "tp_summarize_index_to_output"
assert_no_lazy_graph_walk "${DUMP_SOURCE}" "tp_dump_index_to_output"
assert_no_lazy_graph_walk "${DUMP_SOURCE}" "count_segments"
assert_no_lazy_graph_walk "${DUMP_SOURCE}" "collect_segment_info"
assert_no_lazy_graph_walk "${DUMP_SOURCE}" "mark_segment_pages"
assert_no_lazy_graph_walk "${VACUUM_SOURCE}" "tp_count_live_docs"
assert_no_lazy_graph_walk "${VACUUM_SOURCE}" "tp_vacuum_identify_affected"
assert_recyclable_suffix_truncation

echo "Published graph source guards passed"
