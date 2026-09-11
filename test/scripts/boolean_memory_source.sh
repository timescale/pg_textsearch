#!/bin/bash
#
# Guard that Boolean memtable evaluation streams compact posting arrays instead
# of materializing one CTID hash per term plus a duplicate candidate hash.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BOOLEAN_SOURCE="${REPO_ROOT}/src/access/boolean.c"

if grep -Fq "TpBooleanTerm" "${BOOLEAN_SOURCE}" &&
    grep -Fq "HTAB *ctids" "${BOOLEAN_SOURCE}"; then
    echo "Boolean memtable terms still materialize complete CTID hashes" >&2
    exit 1
fi

if grep -Fq '"BM25 Boolean candidates"' "${BOOLEAN_SOURCE}"; then
    echo "Boolean memtable evaluation still materializes a candidate hash" >&2
    exit 1
fi

if ! grep -Fq "tp_boolean_create_memtable_candidate_stream" \
    "${BOOLEAN_SOURCE}"; then
    echo "Boolean memtable evaluation is not driven by a candidate stream" >&2
    exit 1
fi

echo "Boolean memtable memory source guards passed"
