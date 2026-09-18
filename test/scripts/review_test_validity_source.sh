#!/bin/bash
#
# Guard deterministic lock-order gating and reader-scoped conflict evidence.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOG_SOURCE="${REPO_ROOT}/src/memtable/log.c"
MOD_SOURCE="${REPO_ROOT}/src/mod.c"
VACUUM_TEST="${REPO_ROOT}/test/scripts/vacuum_concurrent_merge.sh"
STANDBY_TEST="${REPO_ROOT}/test/scripts/standby_reclaim.sh"
CONFLICT_HELPER="${REPO_ROOT}/test/scripts/standby_conflict_output.sh"

gate_body="$(
    sed -n '/^tp_debug_gate_memtable_extend(/,/^}/p' "${LOG_SOURCE}"
)"
extend_body="$(
    sed -n '/^memtable_extend_and_append(/,/^}/p' "${LOG_SOURCE}"
)"
conflict_body="$(
    sed -n '/^snapshot_reader_conflict_output_is_valid()/,/^}/p' \
        "${CONFLICT_HELPER}"
)"

if [[ -z "${gate_body}" || -z "${extend_body}" ||
      -z "${conflict_body}" ]]; then
    echo "could not extract test validity helpers" >&2
    exit 1
fi

gate_line="$(
    grep -nF "tp_debug_gate_memtable_extend(rel)" <<<"${extend_body}" |
        cut -d: -f1
)"
metapage_line="$(
    grep -nF "metabuf = ReadBuffer(rel, TP_METAPAGE_BLKNO)" \
        <<<"${extend_body}" | cut -d: -f1
)"
if [[ -z "${gate_line}" || -z "${metapage_line}" ||
      "${gate_line}" -ge "${metapage_line}" ]]; then
    echo "memtable extension gate is not before metapage acquisition" >&2
    exit 1
fi

if ! grep -Fq "LockAcquire(" <<<"${gate_body}" ||
   ! grep -Fq "tp_debug_memtable_extend_gate" <<<"${gate_body}" ||
   grep -Fq "pg_usleep" <<<"${gate_body}" ||
   grep -Fq "debug_memtable_pause_before_extend_ms" \
        "${LOG_SOURCE}" "${MOD_SOURCE}"; then
    echo "memtable extension still uses a timed pause instead of a gate" >&2
    exit 1
fi

if ! grep -Fq "reader.wait_event = 'BufferContent'" "${VACUUM_TEST}" ||
   ! grep -Fq "writer.wait_event = 'advisory'" "${VACUUM_TEST}" ||
   ! grep -Fq "pg_advisory_unlock" "${VACUUM_TEST}"; then
    echo "lock-order test does not prove reader wait state before release" >&2
    exit 1
fi

if grep -Fq 'STANDBY_DIR}/log/postgres.log' <<<"${conflict_body}" ||
   ! grep -Fq "canceling statement due to conflict with recovery" \
        <<<"${conflict_body}" ||
   ! grep -Fq "magic mismatch" <<<"${conflict_body}"; then
    echo "standby conflict proof is not scoped to reader output" >&2
    exit 1
fi

stale_output="${REPO_ROOT}/test/.stale-conflict-output.$$"
corrupt_output="${REPO_ROOT}/test/.corrupt-conflict-output.$$"
valid_output="${REPO_ROOT}/test/.valid-conflict-output.$$"
trap 'rm -f "${stale_output}" "${corrupt_output}" "${valid_output}"' EXIT

# shellcheck source=standby_conflict_output.sh
source "${CONFLICT_HELPER}"
printf '%s\n' \
    "ERROR: terminating connection due to conflict with recovery" \
    >"${stale_output}"
printf '%s\n' \
    "ERROR: canceling statement due to conflict with recovery" \
    "DETAIL: invalid magic on reused memtable page" >"${corrupt_output}"
printf '%s\n' \
    "ERROR: canceling statement due to conflict with recovery" \
    >"${valid_output}"

if snapshot_reader_conflict_output_is_valid "${stale_output}"; then
    echo "stale unrelated conflict output was accepted" >&2
    exit 1
fi
if snapshot_reader_conflict_output_is_valid "${corrupt_output}"; then
    echo "corrupt-page output was accepted as a clean conflict" >&2
    exit 1
fi
if ! snapshot_reader_conflict_output_is_valid "${valid_output}"; then
    echo "reader-scoped recovery conflict output was rejected" >&2
    exit 1
fi

echo "Review test-validity source guards passed"
