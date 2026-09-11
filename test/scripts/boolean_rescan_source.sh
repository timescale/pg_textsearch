#!/bin/bash
#
# Guard PostgreSQL's amrescan contract: NULL scan keys restart with the
# previously supplied keys, so Boolean query state must survive that restart.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SCAN_SOURCE="${REPO_ROOT}/src/access/scan.c"
BOOLEAN_SOURCE="${REPO_ROOT}/src/access/boolean.c"

rescan_body="$(
    sed -n '/^tp_rescan(/,/^}/p' "${SCAN_SOURCE}"
)"
boolean_rescan_body="$(
    sed -n '/^tp_boolean_rescan(/,/^}/p' "${BOOLEAN_SOURCE}"
)"

if [[ -z "${rescan_body}" || -z "${boolean_rescan_body}" ]]; then
    echo "could not extract Boolean rescan function bodies" >&2
    exit 1
fi

replacement_block="$(
    awk '
        /^[[:space:]]*if \(keys != NULL\)$/ {
            capture = 1
        }
        capture {
            print
            opens = gsub(/\{/, "{")
            closes = gsub(/\}/, "}")
            depth += opens - closes
            if (opens > 0)
                saw_open = 1
            if (saw_open && depth == 0)
                exit
        }
    ' <<<"${rescan_body}"
)"

if [[ -z "${replacement_block}" ]] ||
    ! grep -Fq "pfree(so->boolean_query)" <<<"${replacement_block}" ||
    ! grep -Fq "so->boolean_query = NULL" <<<"${replacement_block}" ||
    ! grep -Fq "so->is_boolean_scan = false" <<<"${replacement_block}" ||
    ! grep -Fq "so->boolean_recheck = false" <<<"${replacement_block}"; then
    echo "NULL-key rescans do not preserve copied Boolean scan state" >&2
    exit 1
fi

if [[ "$(grep -Fc "MemoryContextReset(so->boolean_context)" \
        <<<"${rescan_body}")" -ne 1 ]]; then
    echo "Boolean execution scratch is not reset on every rescan" >&2
    exit 1
fi

if grep -Fq "MemoryContextReset(so->boolean_context)" \
    <<<"${boolean_rescan_body}"; then
    echo "copied Boolean query does not outlive execution scratch resets" >&2
    exit 1
fi

unique_line() {
    local body=$1
    local pattern=$2
    local lines

    lines="$(grep -nF "${pattern}" <<<"${body}" || true)"
    if [[ "$(wc -l <<<"${lines}")" -ne 1 || -z "${lines}" ]]; then
        echo "expected exactly one Boolean rescan match for: ${pattern}" >&2
        exit 1
    fi
    cut -d: -f1 <<<"${lines}"
}

switch_line="$(
    unique_line "${boolean_rescan_body}" \
        "MemoryContextSwitchTo(so->scan_context)"
)"
copy_line="$(
    unique_line "${boolean_rescan_body}" \
        "so->boolean_query = DatumGetTSQueryCopy"
)"
restore_line="$(
    unique_line "${boolean_rescan_body}" \
        "MemoryContextSwitchTo(old_context)"
)"

if [[ "${switch_line}" -ge "${copy_line}" ||
      "${copy_line}" -ge "${restore_line}" ]]; then
    echo "copied Boolean query is not owned by the scan context" >&2
    exit 1
fi

echo "Boolean NULL-key rescan source guards passed"
