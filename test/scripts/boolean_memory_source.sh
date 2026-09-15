#!/bin/bash
#
# Guard that Boolean memtable evaluation walks one bounded chain snapshot
# instead of materializing complete posting or document arrays.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BOOLEAN_SOURCE="${REPO_ROOT}/src/access/boolean.c"
WALKER_SOURCE="${REPO_ROOT}/src/memtable/chain_walker.c"

if grep -Fq "TpBooleanTerm" "${BOOLEAN_SOURCE}" &&
    grep -Fq "HTAB *ctids" "${BOOLEAN_SOURCE}"; then
    echo "Boolean memtable terms still materialize complete CTID hashes" >&2
    exit 1
fi

if grep -Fq '"BM25 Boolean candidates"' "${BOOLEAN_SOURCE}"; then
    echo "Boolean memtable evaluation still materializes a candidate hash" >&2
    exit 1
fi

if grep -Fq "tp_boolean_create_memtable_candidate_stream" \
    "${BOOLEAN_SOURCE}" ||
    grep -Fq "tp_boolean_collect_memtable_terms" "${BOOLEAN_SOURCE}" ||
    grep -Fq "tp_boolean_collect_memtable_documents" "${BOOLEAN_SOURCE}"; then
    echo "Boolean memtable evaluation still materializes posting arrays" >&2
    exit 1
fi

if ! grep -Fq "tp_chain_walker_open_bounded" "${BOOLEAN_SOURCE}" ||
    ! grep -Fq "tp_chain_walker_next" "${BOOLEAN_SOURCE}"; then
    echo "Boolean memtable evaluation does not stream a bounded chain snapshot" >&2
    exit 1
fi

if ! grep -Eq "w->copy_records[[:space:]]*=[[:space:]]*true" \
    "${WALKER_SOURCE}" ||
    ! grep -Fq "release_cur_page(w)" "${WALKER_SOURCE}"; then
    echo "bounded Boolean walks can retain a page buffer lock" >&2
    exit 1
fi

echo "Boolean memtable memory source guards passed"
