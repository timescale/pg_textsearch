#!/bin/bash
#
# Guard transaction-dispatch invariants that cannot be observed reliably from
# SQL without exposing test-only state.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
REQUEST_SOURCE="${REPO_ROOT}/src/index/compaction_request.c"
MODULE_SOURCE="${REPO_ROOT}/src/mod.c"
STATE_SOURCE="${REPO_ROOT}/src/index/state.c"
JOB_SOURCE="${REPO_ROOT}/src/index/compaction_job.c"
FRESH_SQL="${REPO_ROOT}/sql/pg_textsearch--1.5.0-dev.sql"
UPGRADE_SQL="${REPO_ROOT}/sql/pg_textsearch--1.4.0--1.5.0-dev.sql"

for required in \
    "df.wait_for_signal" \
    "df.wait_for_schedule" \
    "df.explain" \
    "bm25_compact_step_if_current" \
    "{sys_instance_id}" \
    "DEPENDENCY_NORMAL" \
    "AccessMethodRelationId" \
    "OPERATOR(pg_catalog.=)" \
    "OPERATOR(pg_catalog.~~)" \
    "ANY (ARRAY["; do
    if ! grep -Fq "${required}" "${JOB_SOURCE}"; then
        echo "managed compaction source is missing ${required}" >&2
        exit 1
    fi
done

if ! grep -Fq "tp_compaction_job_preflight" "${JOB_SOURCE}" ||
    ! grep -Fq "tp_compaction_job_preflight" "${MODULE_SOURCE}"; then
    echo "background CIC admission is not preflighted" >&2
    exit 1
fi

if grep -Fq " IN (" "${JOB_SOURCE}"; then
    echo "managed compaction SQL contains a search-path-sensitive IN" >&2
    exit 1
fi

if grep -Fq "bm25_compact_pending" "${FRESH_SQL}" "${UPGRADE_SQL}"; then
    echo "database-wide background compaction sweep remains installed" >&2
    exit 1
fi

if grep -Eq '#include[[:space:]]*[<"].*pg_durable' "${JOB_SOURCE}"; then
    echo "managed compaction has a build-time pg_durable header dependency" >&2
    exit 1
fi

if grep -Fq "tp_compaction_drop_request(objectId)" "${MODULE_SOURCE}"; then
    echo "DROP removes pending requests before subtransaction outcome" >&2
    exit 1
fi

if ! grep -Fq "SearchSysCacheExists1" "${REQUEST_SOURCE}" ||
    ! grep -Fq "RELOID, ObjectIdGetDatum(indexoid)" "${REQUEST_SOURCE}"; then
    echo "pending requests are not revalidated before dispatch" >&2
    exit 1
fi

# Pending requests must live in TopTransactionContext, which PostgreSQL
# frees at commit, prepare, and abort alike.  TopMemoryContext would
# outlive the transaction and leak stale OIDs into the next one, and would
# need a discard at every transaction end, including PREPARE.
request_body="$(
    sed -n '/^tp_compaction_request(Oid indexoid)/,/^}/p' "${REQUEST_SOURCE}"
)"
if ! grep -Fq "MemoryContextSwitchTo(TopTransactionContext)" \
    <<<"${request_body}"; then
    echo "pending requests are not allocated in TopTransactionContext" >&2
    exit 1
fi
if grep -Fq "TopMemoryContext" <<<"${request_body}"; then
    echo "pending requests outlive the recording transaction" >&2
    exit 1
fi
if ! grep -Fq "MemoryContextRegisterResetCallback" <<<"${request_body}"; then
    echo "pending request list pointer is not cleared with its context" >&2
    exit 1
fi

# Running callback SQL at PRE_PREPARE can leave transaction-global state
# (notably XACT_FLAGS_ACCESSEDTEMPNAMESPACE) that PostgreSQL validates
# after the event and that subtransaction rollback cannot clear, making an
# otherwise valid PREPARE TRANSACTION fail.
preprepare_body="$(
    sed -n '/case XACT_EVENT_PRE_PREPARE:/,/break;/p' "${MODULE_SOURCE}"
)"
if grep -Fq "tp_compaction_flush_requests" <<<"${preprepare_body}"; then
    echo "PRE_PREPARE dispatches callback SQL" >&2
    exit 1
fi

# A spill caused during dispatch must compact inline: its request would land
# in a list the running dispatch has already stopped reading.
if ! grep -Fq "tp_dispatch_active = true" "${REQUEST_SOURCE}" ||
    ! grep -Fq "tp_dispatch_active" "${REQUEST_SOURCE}"; then
    echo "managed dispatch re-entry is not routed to inline compaction" >&2
    exit 1
fi

flush_body="$(
    sed -n '/^tp_compaction_flush_requests(void)/,$p' "${REQUEST_SOURCE}"
)"
revalidate_line="$(grep -n "SearchSysCacheExists1" <<<"${flush_body}" |
    cut -d: -f1)"
signal_line="$(grep -n "tp_run_request(indexoid)" \
    <<<"${flush_body}" | cut -d: -f1)"

if [[ -z "${revalidate_line}" || -z "${signal_line}" ||
      "${revalidate_line}" -ge "${signal_line}" ]]; then
    echo "pending requests are not revalidated before signaling" >&2
    exit 1
fi

bulk_spill_body="$(
    sed -n '/^tp_bulk_load_spill_check(void)/,/^tp_reset_bulk_load_counters(void)/p' \
        "${STATE_SOURCE}"
)"
open_line="$(grep -n "index_rel = try_index_open" <<<"${bulk_spill_body}" |
    cut -d: -f1)"
acquire_line="$(grep -n "tp_acquire_index_lock(local_state" \
    <<<"${bulk_spill_body}" | cut -d: -f1)"
release_line="$(grep -n "tp_release_index_lock(local_state" \
    <<<"${bulk_spill_body}" | tail -1 | cut -d: -f1)"
close_line="$(grep -n "index_close(index_rel" <<<"${bulk_spill_body}" |
    cut -d: -f1)"

if [[ -z "${open_line}" || -z "${acquire_line}" ||
      "${open_line}" -ge "${acquire_line}" ]]; then
    echo "bulk spill does not open the relation before its index LWLock" >&2
    exit 1
fi

if [[ -z "${release_line}" || -z "${close_line}" ||
      "${release_line}" -ge "${close_line}" ]]; then
    echo "bulk spill does not release its index LWLock before relation close" >&2
    exit 1
fi

echo "Compaction request source guards passed"
